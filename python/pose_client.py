"""Asking the pose server where the mouse is looking, from a behaviour protocol.

Standard library only. The behaviour system should not grow a dependency on
PyTorch to ask a question.

**One call, everything derived from one frame.** A snapshot carries the heading,
the angle to every port, and the position, all computed from the same image and
sharing one ``frame_id``. A protocol then chooses a port by looking at the
geometry rather than guessing one and asking about it:

    snap = self.pose.snapshot()
    if snap.ok:
        target = snap.port_ahead(within=45)

**The pose system only reports what it saw.** It has no idea which port a
protocol went on to cue, what trial it is, or what happened next, and there is no
way to tell it. That keeps protocols free of bookkeeping calls and keeps the
session's pose log honestly a record of perception. Anything about decisions or
outcomes belongs in the protocol's own trial record, which joins to this one on
``frame_id``.

**No failure here raises into a protocol.** A dead server, a timeout, a mouse the
model cannot find -- all come back as ``ok == False`` with a reason. A protocol
that wants to abort a trial on a missed reading can; one that wants to fall
through to a default can; neither needs try/except, and a missing pose reading
can never take down a session.
"""

from __future__ import annotations

import json
import socket
import threading
from dataclasses import dataclass, field
from typing import Any, Optional


def _wrap_180(angle: float) -> float:
    """Fold an angle into (-180, 180], the form a protocol wants to compare."""
    angle = angle % 360
    if angle > 180:
        return angle - 360
    return angle


@dataclass
class Snapshot:
    """One look at the mouse, and everything the image implies about it.

    ``ok`` means the model found the mouse confidently and the frame was fresh
    enough to act on. The other fields are still filled in where they are known
    even when it is False, because a refused reading with a bearing and a low
    likelihood says far more about what went wrong than a bare False.
    """
    ok: bool
    reason: Optional[str] = None

    # What the mouse is doing.
    heading: Optional[float] = None               # degrees, 0-360
    position: Optional[tuple[float, float]] = None   # px, full frame
    distance_from_centre: Optional[float] = None     # px
    position_mm: Optional[tuple[float, float]] = None   # if the rig is calibrated

    # Where everything is relative to it. Keys are 1-based port numbers.
    port_angles: dict[int, float] = field(default_factory=dict)      # signed, -180..180
    port_distances: dict[int, float] = field(default_factory=dict)   # px

    # Provenance.
    frame_id: Optional[int] = None                # joins to the video and the DAQ
    snapshot_id: Optional[int] = None
    frame_age_ms: Optional[float] = None
    inference_ms: Optional[float] = None
    round_trip_ms: Optional[float] = None

    # How much to trust it.
    angle_correction_method: Optional[str] = None
    min_likelihood: Optional[float] = None
    spine_data_available: Optional[bool] = None
    ear_distance: Optional[float] = None

    keypoints: dict[str, list[float]] = field(default_factory=dict)
    raw: dict[str, Any] = field(default_factory=dict)

    # ----- asking about ports -----

    def angle_to(self, port: int) -> Optional[float]:
        """Signed angle from the mouse's heading to a 1-based port, (-180, 180].

        Negative one way, positive the other; what a protocol usually wants is
        ``abs()`` of it against a tolerance.
        """
        return self.port_angles.get(int(port))

    def distance_to(self, port: int) -> Optional[float]:
        """How far the mouse is from a port, in pixels."""
        return self.port_distances.get(int(port))

    def facing(self, port: int, within: float = 30.0) -> bool:
        """Is the mouse looking at this port, within a tolerance?

        False when there is no reading, so ``if snap.facing(3):`` is safe without
        checking ``ok`` first: a missing reading is not a mouse facing the port.
        """
        if not self.ok:
            return False
        angle = self.angle_to(port)
        return angle is not None and abs(angle) <= within

    def ports_by_angle(self) -> list[tuple[int, float]]:
        """Every port as (port, angle), most closely aligned first."""
        return sorted(self.port_angles.items(), key=lambda kv: abs(kv[1]))

    def port_ahead(self, within: float = 60.0) -> Optional[int]:
        """The port the mouse is most nearly looking at, or None if none is close.

        This is about *direction*. For the port it is physically beside, use
        ``closest_port()`` -- the two are different questions and agree often
        enough to hide a mistake.
        """
        if not self.ok:
            return None
        ordered = self.ports_by_angle()
        if not ordered or abs(ordered[0][1]) > within:
            return None
        return ordered[0][0]

    def closest_port(self) -> Optional[int]:
        """The port the mouse is physically nearest, by distance, not direction."""
        if not self.ok or not self.port_distances:
            return None
        return min(self.port_distances.items(), key=lambda kv: kv[1])[0]


class PoseClient:
    """A connection to one rig's pose server.

    Reconnects by itself, because the server may be restarted mid-session and a
    protocol should not have to care. Safe to call from more than one thread.
    """

    def __init__(self, host: str = "127.0.0.1", port: int = 5801,
                 *, connect_timeout_s: float = 2.0):
        self.host = host
        self.port = port
        self.connect_timeout_s = connect_timeout_s
        self._socket: Optional[socket.socket] = None
        self._file = None
        self._lock = threading.Lock()

    # ----- connection -----

    def _connect(self, timeout_s: float) -> None:
        sock = socket.create_connection((self.host, self.port),
                                        timeout=self.connect_timeout_s)
        sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        sock.settimeout(timeout_s)
        self._socket = sock
        self._file = sock.makefile("rwb")

    def _disconnect(self) -> None:
        for item in (self._file, self._socket):
            try:
                if item is not None:
                    item.close()
            except Exception:                                 # noqa: BLE001
                pass
        self._file = None
        self._socket = None

    def _request(self, payload: dict, timeout_s: float) -> dict:
        """One round trip, with a single reconnect-and-retry.

        The retry is there because the first request after the server restarts
        would otherwise fail on a socket that looks open and is not.
        """
        for attempt in (0, 1):
            try:
                if self._socket is None:
                    self._connect(timeout_s)
                else:
                    self._socket.settimeout(timeout_s)

                self._file.write((json.dumps(payload) + "\n").encode("utf-8"))
                self._file.flush()
                line = self._file.readline()
                if not line:
                    raise ConnectionError("the pose server closed the connection")
                return json.loads(line.decode("utf-8"))
            except Exception as exc:                          # noqa: BLE001
                self._disconnect()
                if attempt == 1:
                    return {"ok": False, "reason": f"{type(exc).__name__}: {exc}"}
        return {"ok": False, "reason": "unreachable"}

    # ----- the call a protocol makes -----

    def snapshot(self, *, timeout_ms: float = 150.0,
                 max_age_ms: Optional[float] = 100.0) -> Snapshot:
        """Look at the mouse now: heading, position, and the angle to every port.

        ``max_age_ms`` refuses a frame older than this rather than deciding on
        stale data. A protocol can retry; it cannot un-make a decision.
        """
        with self._lock:
            reply = self._request({"cmd": "snapshot", "max_age_ms": max_age_ms},
                                  timeout_s=timeout_ms / 1000.0)
        return self._to_snapshot(reply)

    @staticmethod
    def _to_snapshot(reply: dict) -> Snapshot:
        position = reply.get("position")
        position_mm = reply.get("position_mm")
        # JSON object keys are strings; protocols index by int port number.
        angles = {int(k): v for k, v in (reply.get("port_angles") or {}).items()}
        distances = {int(k): v for k, v in (reply.get("port_distances") or {}).items()}
        return Snapshot(
            ok=bool(reply.get("ok")),
            reason=reply.get("reason"),
            heading=reply.get("heading"),
            position=tuple(position) if position else None,
            distance_from_centre=reply.get("distance_from_centre"),
            position_mm=tuple(position_mm) if position_mm else None,
            port_angles=angles,
            port_distances=distances,
            frame_id=reply.get("frame_id"),
            snapshot_id=reply.get("snapshot_id"),
            frame_age_ms=reply.get("frame_age_ms"),
            inference_ms=reply.get("inference_ms"),
            round_trip_ms=reply.get("round_trip_ms"),
            angle_correction_method=reply.get("angle_correction_method"),
            min_likelihood=reply.get("min_likelihood"),
            spine_data_available=reply.get("spine_data_available"),
            ear_distance=reply.get("ear_distance"),
            keypoints=reply.get("keypoints") or {},
            raw=reply,
        )

    # ----- housekeeping -----

    def status(self, *, timeout_ms: float = 2000.0) -> dict:
        with self._lock:
            return self._request({"cmd": "status"}, timeout_s=timeout_ms / 1000.0)

    def ping(self, *, timeout_ms: float = 1000.0) -> bool:
        with self._lock:
            return bool(self._request({"cmd": "ping"},
                                      timeout_s=timeout_ms / 1000.0).get("ok"))

    def recent(self, since: int = 0, *, want_image: bool = False,
               timeout_ms: float = 3000.0) -> dict:
        """Snapshots taken since ``since``. Used by the viewer, not by protocols."""
        with self._lock:
            return self._request(
                {"cmd": "recent", "since": since, "want_image": want_image},
                timeout_s=timeout_ms / 1000.0)

    def shutdown(self, *, timeout_ms: float = 2000.0) -> bool:
        with self._lock:
            return bool(self._request({"cmd": "shutdown"},
                                      timeout_s=timeout_ms / 1000.0).get("ok"))

    def close(self) -> None:
        with self._lock:
            self._disconnect()

    def __enter__(self) -> "PoseClient":
        return self

    def __exit__(self, *exc_info) -> None:
        self.close()
