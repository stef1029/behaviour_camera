"""Talking to the pose server from a behaviour protocol.

Deliberately small and dependency-free -- standard library only -- because this
is the half that gets imported into the behaviour system, and the behaviour
system should not grow a dependency on PyTorch to ask a question.

The shape of the API is set by what a protocol is: a sequential script. So
``heading()`` blocks with a timeout and returns a result object rather than
raising, for the same reason ``self.sleep()`` looks the way it does. **No failure
here raises into a protocol.** A timeout, a dead server, a mouse the model cannot
find -- all come back as ``ok == False`` with a reason. A protocol that wants to
abort a trial on a missed reading can; one that wants to fall through to a default
can; neither has to wrap anything in try/except, and an unhandled exception can
never take down a session over a missing pose reading.

    pose = PoseClient(port=5801)
    reading = pose.heading(target_port=3, timeout_ms=100)
    if reading.ok and abs(reading.cue_angle) < 30:
        self.link.led_on(3)
"""

from __future__ import annotations

import json
import socket
import threading
from dataclasses import dataclass, field
from typing import Any, Optional


@dataclass
class Reading:
    """One answer from the pose server.

    ``ok`` means the model found the mouse confidently and the frame was fresh
    enough. Everything else is still filled in where it is known, because a
    failed reading with a bearing and a low likelihood is much more useful for
    working out what went wrong than a bare False.
    """
    ok: bool
    reason: Optional[str] = None

    bearing: Optional[float] = None              # degrees, 0-360
    cue_angle: Optional[float] = None            # signed, (-180, 180], to target_port
    midpoint: Optional[tuple[float, float]] = None

    frame_id: Optional[int] = None               # join key to the recorded video
    frame_age_ms: Optional[float] = None
    inference_ms: Optional[float] = None
    round_trip_ms: Optional[float] = None

    angle_correction_method: Optional[str] = None
    min_likelihood: Optional[float] = None
    spine_data_available: Optional[bool] = None

    relative_angles: list[float] = field(default_factory=list)
    keypoints: dict[str, list[float]] = field(default_factory=dict)
    raw: dict[str, Any] = field(default_factory=dict)

    def angle_to_port(self, port: int) -> Optional[float]:
        """Signed angle from the mouse's heading to a 1-based port, (-180, 180].

        Usually wanted as ``abs(...)`` against a tolerance.
        """
        index = int(port) - 1
        if not self.relative_angles or not (0 <= index < len(self.relative_angles)):
            return None
        angle = self.relative_angles[index] % 360
        if angle > 180:
            angle -= 360
        elif angle <= -180:
            angle += 360
        return angle

    def facing(self, port: int, tolerance_deg: float = 30.0) -> bool:
        """Is the mouse looking at this port, within a tolerance?

        False when there is no reading, so ``if reading.facing(3):`` is safe to
        write without checking ``ok`` first -- a missing reading is not a mouse
        that is facing the port.
        """
        if not self.ok:
            return False
        angle = self.angle_to_port(port)
        return angle is not None and abs(angle) <= tolerance_deg


class PoseClient:
    """A connection to one rig's pose server.

    Reconnects by itself: the server may be restarted mid-session, and a protocol
    should not have to care. Safe to call from more than one thread.
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

        The retry exists because the first request after the server restarts
        would otherwise fail on a socket that looks open but is not.
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
                    return {"ok": False,
                            "reason": f"{type(exc).__name__}: {exc}"}
        return {"ok": False, "reason": "unreachable"}

    # ----- the call a protocol makes -----

    def heading(self, target_port: Optional[int] = None, *,
                timeout_ms: float = 150.0,
                max_age_ms: Optional[float] = 100.0) -> Reading:
        """Where is the mouse looking, right now?

        ``target_port`` is 1-based; when given, ``cue_angle`` is the signed angle
        from the mouse's heading to that port. ``max_age_ms`` rejects a frame
        older than this rather than deciding on stale data -- a protocol can
        retry, but it cannot un-make a decision.
        """
        with self._lock:
            reply = self._request(
                {"cmd": "heading", "target_port": target_port,
                 "max_age_ms": max_age_ms},
                timeout_s=timeout_ms / 1000.0)
        return self._to_reading(reply)

    @staticmethod
    def _to_reading(reply: dict) -> Reading:
        midpoint = reply.get("midpoint")
        return Reading(
            ok=bool(reply.get("ok")),
            reason=reply.get("reason"),
            bearing=reply.get("bearing"),
            cue_angle=reply.get("cue_presentation_angle"),
            midpoint=tuple(midpoint) if midpoint else None,
            frame_id=reply.get("frame_id"),
            frame_age_ms=reply.get("frame_age_ms"),
            inference_ms=reply.get("inference_ms"),
            round_trip_ms=reply.get("round_trip_ms"),
            angle_correction_method=reply.get("angle_correction_method"),
            min_likelihood=reply.get("min_likelihood"),
            spine_data_available=reply.get("spine_data_available"),
            relative_angles=reply.get("relative_angles") or [],
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
