"""The live pose server: one process per rig, answering "where is the mouse?"

Sits between the recorder's shared memory and the behaviour system. A protocol
asks for a snapshot; this takes the newest frame, runs the model on it, computes
the heading exactly as the offline analysis would, works out where every port is
relative to the animal, logs it, and replies.

**It only reports what it saw.** The server has no idea which port a protocol
went on to cue, what trial it is, or what happened next, and there is no way to
tell it. That keeps protocols free of bookkeeping calls, and keeps the session's
pose log honestly a record of perception. Decisions and outcomes belong in the
protocol's own trial record, which joins to this one on ``frame_id``.

**Inference is on demand.** A reading is wanted at specific moments -- a cue
decision -- not sixty times a second, so running the model constantly would burn
the GPU for nothing. Between requests the server does almost nothing. The one
exception is the monitor snapshot: while a viewer window is open it takes a slow
background look so the display is not frozen on the last decision. Those are
marked ``monitor`` and are deliberately kept out of the CSV, so the log stays a
record of what the protocol actually asked for.

**A separate process**, because the recording must not be able to suffer: a crash
anywhere in PyTorch or CUDA takes down only this, and nothing it does can stall
the capture thread.

The wire protocol is newline-delimited JSON over a loopback TCP socket. Loopback
costs about 0.1 ms, any language can speak it, and it can be driven by hand from
a terminal when something is wrong -- which matters more than a faster protocol
when a rig is misbehaving at 9pm.

    python python/pose_server.py --rig rig3 --model <path> --port 5803

Requests, one JSON object per line:

    {"cmd": "snapshot", "max_age_ms": 100}
    {"cmd": "recent", "since": 0, "want_image": true}
    {"cmd": "status"} | {"cmd": "ping"} | {"cmd": "shutdown"}
"""

from __future__ import annotations

import argparse
import base64
import csv
import json
import math
import queue
import socket
import socketserver
import sys
import threading
import time
from collections import deque
from pathlib import Path
from typing import Optional

sys.path.insert(0, str(Path(__file__).resolve().parent))

from frame_out_reader import FrameOutReader, FrameOutError, NotRunning  # noqa: E402
from gpu_stats import GpuMonitor                                        # noqa: E402
from head_angle import head_angle                                       # noqa: E402

POSE_PARTS = ("left_ear", "right_ear", "spine_1", "spine_2", "spine_3", "spine_4")

# One row per snapshot the protocol asked for. Everything here is derived from
# the image; nothing is supplied from outside.
LOG_COLUMNS = [
    "snapshot_id", "request_time_unix", "round_trip_ms",
    "frame_id", "frame_sequence", "capture_unix_ns", "frame_age_ms",
    "inference_ms", "crop_x", "crop_y", "crop_size",
    "ok", "reason",
    "heading", "position_x", "position_y", "distance_from_centre",
    "angle_correction_method", "ear_distance", "spine_data_available",
    "min_likelihood", "left_ear_likelihood", "right_ear_likelihood",
]
LOG_COLUMNS += [f"port_{i}_angle" for i in range(1, 7)]
LOG_COLUMNS += [f"port_{i}_distance" for i in range(1, 7)]
for _part in POSE_PARTS:
    LOG_COLUMNS += [f"{_part}_x", f"{_part}_y", f"{_part}_p"]

# How long a viewer's poll keeps the server drawing and encoding. With nothing
# attached, none of that work happens at all.
_VIEWER_ATTACHED_S = 4.0


class ReplayReader:
    """A stand-in for the shared memory block, backed by a folder of images.

    Lets the whole server, client and protocol path be exercised with real mice
    without a rig: there is no way to put a mouse in front of a bench camera, and
    "it works except for the part that sees the animal" is not a tested system.

    Presents the same surface as FrameOutReader, so nothing downstream knows the
    difference. Frames advance on a clock, as a camera's would.
    """

    def __init__(self, folder: str | Path, *, fps: float = 100.0):
        import imageio.v3 as iio
        import numpy as np

        folder = Path(folder)
        paths = sorted(p for p in folder.iterdir()
                       if p.suffix.lower() in (".png", ".jpg", ".jpeg", ".tif", ".bmp"))
        if not paths:
            raise FrameOutError(f"no images to replay in {folder}")

        self.images = []
        for path in paths:
            image = iio.imread(path)
            if image.ndim == 3:
                image = image[..., 0]
            self.images.append(np.ascontiguousarray(image))

        self.height, self.width = self.images[0].shape[:2]
        self.stride = self.width
        self.frame_bytes = self.width * self.height
        self.qpc_frequency = 10_000_000
        self.name = f"replay:{folder}"
        self._fps = fps
        self._started = time.perf_counter()

    def latest(self):
        from frame_out_reader import Frame
        index = int((time.perf_counter() - self._started) * self._fps)
        return Frame(
            image=self.images[index % len(self.images)],
            frame_id=index,
            sequence=index + 1,
            capture_qpc=0,
            capture_unix_ns=int(time.time() * 1e9),
            frames_captured=index + 1,
        )

    def close(self) -> None:
        pass


class PoseService:
    """Everything a request needs, shared between connections under one lock.

    The lock is not a performance worry: requests are occasional, the GPU can
    only run one inference at a time anyway, and serialising them keeps the
    latency figures honest rather than hiding queueing inside a batch.
    """

    def __init__(self, engine, reader_factory, *, port_coordinates=None,
                 crop_size: Optional[int] = 640, min_likelihood: float = 0.6,
                 log_path: Optional[Path] = None, keepalive_s: float = 0.5,
                 mm_per_pixel: Optional[float] = None,
                 snapshot_dir: Optional[Path] = None,
                 monitor_fps: float = 2.0, history: int = 60):
        self.engine = engine
        self._reader_factory = reader_factory
        self._reader = None
        self.port_coordinates = port_coordinates
        self.crop_size = crop_size
        self.min_likelihood = min_likelihood
        self.keepalive_s = keepalive_s
        self.mm_per_pixel = mm_per_pixel
        self.snapshot_dir = snapshot_dir
        self.monitor_fps = monitor_fps

        self._lock = threading.Lock()
        self._stop = threading.Event()
        self._last_activity = time.perf_counter()
        self._last_shape: Optional[tuple[int, int]] = None

        self._snapshot_id = 0
        self.requests = 0
        self.failures = 0
        self.started_unix = time.time()
        self._latencies: deque = deque(maxlen=200)

        # What the viewer reads. Metadata for everything; the image only for the
        # most recent one, and only while someone is looking.
        self._history: deque = deque(maxlen=history)
        self._latest_raw = None            # (crop image, crop, keypoints, result)
        self._latest_jpeg: Optional[bytes] = None
        self._latest_jpeg_id = -1
        self._viewer_seen_at = 0.0

        self.gpu = GpuMonitor()
        self._gpu_sample = self.gpu.sample()
        self._gpu_sampled_at = 0.0

        self._log_file = None
        self._log_writer = None
        self._log_queue: queue.Queue = queue.Queue()
        self._log_thread: Optional[threading.Thread] = None
        if log_path is not None:
            log_path.parent.mkdir(parents=True, exist_ok=True)
            self._log_file = open(log_path, "w", newline="", encoding="utf-8")
            self._log_writer = csv.DictWriter(self._log_file, fieldnames=LOG_COLUMNS)
            self._log_writer.writeheader()
            self._log_file.flush()
            self._log_thread = threading.Thread(target=self._log_loop, daemon=True)
            self._log_thread.start()

    # ----- frame source -----

    def _ensure_reader(self):
        """Attach lazily, and re-attach if the recorder restarted."""
        if self._reader is None:
            self._reader = self._reader_factory()
        return self._reader

    def _drop_reader(self) -> None:
        if self._reader is not None:
            try:
                self._reader.close()
            except Exception:                                 # noqa: BLE001
                pass
            self._reader = None

    @property
    def viewer_attached(self) -> bool:
        return (time.perf_counter() - self._viewer_seen_at) < _VIEWER_ATTACHED_S

    # ----- the request -----

    def snapshot(self, *, max_age_ms: Optional[float] = None,
                 source: str = "request") -> dict:
        request_time = time.time()
        started = time.perf_counter()

        with self._lock:
            self._last_activity = started
            self._snapshot_id += 1
            snapshot_id = self._snapshot_id
            if source == "request":
                self.requests += 1
            try:
                reply = self._snapshot_locked(max_age_ms)
            except NotRunning as exc:
                self._drop_reader()
                reply = {"ok": False, "reason": f"recorder not running: {exc}"}
            except FrameOutError as exc:
                self._drop_reader()
                reply = {"ok": False, "reason": f"frame out error: {exc}"}
            except Exception as exc:                          # noqa: BLE001
                reply = {"ok": False, "reason": f"{type(exc).__name__}: {exc}"}

        reply["snapshot_id"] = snapshot_id
        reply["source"] = source
        reply["round_trip_ms"] = round((time.perf_counter() - started) * 1000.0, 3)
        reply["request_time_unix"] = request_time

        if source == "request":
            if not reply.get("ok"):
                self.failures += 1
            self._latencies.append(reply["round_trip_ms"])
            self._log(reply)

        self._remember(reply)
        return reply

    def _snapshot_locked(self, max_age_ms) -> dict:
        reader = self._ensure_reader()
        frame = reader.latest()
        if frame is None:
            return {"ok": False, "reason": "no frame has been published yet"}

        age_ms = (time.time() * 1e9 - frame.capture_unix_ns) / 1e6
        if max_age_ms is not None and age_ms > max_age_ms:
            # Better to say the data is too old than to decide on it. A protocol
            # can retry; it cannot un-make a decision.
            return {"ok": False, "reason": f"newest frame is {age_ms:.1f} ms old",
                    "frame_id": frame.frame_id, "frame_age_ms": round(age_ms, 2)}

        keypoints = self.engine.infer(frame.image, crop_size=self.crop_size)
        crop = getattr(self.engine, "last_crop", None)
        if crop is not None:
            self._last_shape = (crop.height, crop.width)

        result = head_angle(keypoints, port_coordinates=self.port_coordinates)
        height, width = frame.image.shape[:2]

        base = {
            "frame_id": frame.frame_id,
            "frame_sequence": frame.sequence,
            "capture_unix_ns": frame.capture_unix_ns,
            "frame_age_ms": round(age_ms, 2),
            "inference_ms": round(self.engine.last_inference_ms, 3),
            "keypoints": {
                name: [round(k.x, 2), round(k.y, 2), round(k.likelihood, 4)]
                for name, k in keypoints.items()
            },
        }
        if crop is not None:
            base["crop"] = [crop.x, crop.y, crop.width, crop.height]

        # Keep the crop and keypoints for the viewer to draw, if one is looking.
        # A ~0.4 MB copy, about 0.1 ms; the drawing and encoding happen on the
        # render thread, off the request path entirely.
        if self.viewer_attached and crop is not None:
            try:
                self._latest_raw = (
                    frame.image[crop.y:crop.y + crop.height,
                                crop.x:crop.x + crop.width].copy(),
                    crop, keypoints, result)
            except Exception:                                 # noqa: BLE001
                self._latest_raw = None

        if result is None:
            return {**base, "ok": False, "reason": "no ears found"}

        confident = result.min_likelihood >= self.min_likelihood

        angles, distances = {}, {}
        for index, angle in enumerate(result.relative_angles, start=1):
            folded = angle % 360
            angles[index] = round(folded - 360 if folded > 180 else folded, 3)
        if self.port_coordinates:
            for index, (px, py) in enumerate(self.port_coordinates, start=1):
                distances[index] = round(math.hypot(px - result.midpoint[0],
                                                    py - result.midpoint[1]), 1)

        centre_distance = math.hypot(result.midpoint[0] - width / 2,
                                     result.midpoint[1] - height / 2)

        reply = {
            **base,
            "ok": confident,
            "reason": None if confident else
                      f"low confidence ({result.min_likelihood:.2f} < "
                      f"{self.min_likelihood:.2f})",
            "heading": round(result.bearing, 3),
            "position": [round(result.midpoint[0], 2), round(result.midpoint[1], 2)],
            "distance_from_centre": round(centre_distance, 1),
            "port_angles": angles,
            "port_distances": distances,
            "angle_correction_method": result.angle_correction_method,
            "ear_distance": round(result.ear_distance, 2),
            "spine_data_available": result.spine_data_available,
            "min_likelihood": round(result.min_likelihood, 4),
            "left_ear_likelihood": round(result.left_ear_likelihood, 4),
            "right_ear_likelihood": round(result.right_ear_likelihood, 4),
        }
        if self.mm_per_pixel:
            reply["position_mm"] = [
                round((result.midpoint[0] - width / 2) * self.mm_per_pixel, 1),
                round((result.midpoint[1] - height / 2) * self.mm_per_pixel, 1),
            ]
        return reply

    # ----- what the viewer reads -----

    def _remember(self, reply: dict) -> None:
        """A compact record of every snapshot, for the history table."""
        self._history.append({
            "snapshot_id": reply.get("snapshot_id"),
            "source": reply.get("source"),
            "time_unix": reply.get("request_time_unix"),
            "frame_id": reply.get("frame_id"),
            "ok": reply.get("ok"),
            "reason": reply.get("reason"),
            "heading": reply.get("heading"),
            "position": reply.get("position"),
            "distance_from_centre": reply.get("distance_from_centre"),
            "port_angles": reply.get("port_angles"),
            "port_distances": reply.get("port_distances"),
            "angle_correction_method": reply.get("angle_correction_method"),
            "min_likelihood": reply.get("min_likelihood"),
            "inference_ms": reply.get("inference_ms"),
            "round_trip_ms": reply.get("round_trip_ms"),
            "frame_age_ms": reply.get("frame_age_ms"),
        })

    def recent(self, since: int = 0, want_image: bool = False) -> dict:
        """Snapshots after ``since``, for the viewer. Marks a viewer as attached."""
        self._viewer_seen_at = time.perf_counter()
        rows = [r for r in self._history if (r["snapshot_id"] or 0) > since]
        out = {
            "ok": True,
            "latest_id": self._snapshot_id,
            "snapshots": rows,
            "gpu": self._gpu().to_dict(),
            "stats": self._stats(),
        }
        if want_image and self._latest_jpeg is not None:
            out["image_id"] = self._latest_jpeg_id
            out["image_jpeg_b64"] = base64.b64encode(self._latest_jpeg).decode("ascii")
        return out

    def _gpu(self):
        now = time.perf_counter()
        if now - self._gpu_sampled_at > 0.4:
            self._gpu_sample = self.gpu.sample()
            self._gpu_sampled_at = now
        return self._gpu_sample

    def _stats(self) -> dict:
        latencies = sorted(self._latencies)
        median = latencies[len(latencies) // 2] if latencies else 0.0
        return {
            "uptime_s": round(time.time() - self.started_unix, 1),
            "requests": self.requests,
            "failures": self.failures,
            "found_percent": round(
                100.0 * (self.requests - self.failures) / self.requests, 1)
            if self.requests else 0.0,
            "median_round_trip_ms": round(median, 2),
            "crop_size": self.crop_size,
            "frame_out_attached": self._reader is not None,
        }

    # ----- background work -----

    def render_loop(self) -> None:
        """Draw and JPEG-encode the latest snapshot, for the viewer.

        Deliberately not on the request path: a protocol waiting on a heading
        should never be waiting on a JPEG. Nothing happens here unless a viewer
        has polled recently.
        """
        import copy
        import cv2
        from pose_overlay import draw_pose

        while not self._stop.wait(0.15):
            if not self.viewer_attached:
                continue
            with self._lock:
                item = self._latest_raw
                snapshot_id = self._snapshot_id
            if item is None or snapshot_id == self._latest_jpeg_id:
                continue
            crop_image, crop, keypoints, result = item
            try:
                # Keypoints are in full-frame space; shift them into the crop so
                # they land correctly on the cropped image the viewer shows.
                shifted = {
                    name: type(point)(point.x - crop.x, point.y - crop.y,
                                      point.likelihood)
                    for name, point in keypoints.items()
                }
                local = None
                if result is not None:
                    local = copy.copy(result)
                    local.midpoint = (result.midpoint[0] - crop.x,
                                      result.midpoint[1] - crop.y)
                    local.ear_midpoint = (result.ear_midpoint[0] - crop.x,
                                          result.ear_midpoint[1] - crop.y)
                canvas = draw_pose(crop_image, shifted, local,
                                   min_likelihood=self.min_likelihood, scale=1.2)
                ok, buffer = cv2.imencode(".jpg", canvas,
                                          [int(cv2.IMWRITE_JPEG_QUALITY), 72])
                if ok:
                    self._latest_jpeg = buffer.tobytes()
                    self._latest_jpeg_id = snapshot_id
                    if self.snapshot_dir is not None:
                        self._save_snapshot(snapshot_id, self._latest_jpeg)
            except Exception:                                 # noqa: BLE001
                pass

    def _save_snapshot(self, snapshot_id: int, jpeg: bytes) -> None:
        try:
            self.snapshot_dir.mkdir(parents=True, exist_ok=True)
            (self.snapshot_dir / f"snapshot_{snapshot_id:06d}.jpg").write_bytes(jpeg)
        except Exception:                                     # noqa: BLE001
            pass

    def monitor_loop(self) -> None:
        """A slow snapshot while a viewer is open, so the window is not frozen.

        Only while someone is watching, and marked ``monitor`` so it never lands
        in the CSV: the log is a record of what the protocol asked for, and
        padding it with display frames would make the request count meaningless.
        """
        if self.monitor_fps <= 0:
            return
        period = 1.0 / self.monitor_fps
        while not self._stop.wait(period):
            if not self.viewer_attached:
                continue
            if time.perf_counter() - self._last_activity < period:
                continue        # a real request just ran; nothing to add
            try:
                self.snapshot(max_age_ms=None, source="monitor")
            except Exception:                                 # noqa: BLE001
                pass

    def keepalive_loop(self) -> None:
        """A throwaway inference now and then, so the GPU does not clock down.

        A fallback, not the fix. An idle GPU drops its graphics clock to a few
        hundred MHz and its memory clock with it, and an inference once a second
        then costs about ten times one run back to back -- 94 ms against 8 ms,
        measured. The inter-trial interval is exactly such a gap.

        The real fix is locking both clocks, which makes latency flat at any
        request rate for about 15 W: ``scripts/configure_rig.ps1 -Apply``. That
        needs administrator, so this process cannot do it, and falls back to
        keeping the GPU awake by working it. Measured, that needs an inference
        every 50 ms or so to be worth anything, which is a real slice of the GPU
        spent on nothing. Hence: cheap when the clocks are locked, expensive only
        when it has to be.
        """
        import numpy as np
        blank = None
        blank_shape = None
        while not self._stop.wait(self.keepalive_s):
            if time.perf_counter() - self._last_activity < self.keepalive_s:
                continue
            shape = self._last_shape
            if shape is None:
                continue        # nothing real has run, so the shape is unknown
            with self._lock:
                try:
                    if blank is None or blank_shape != shape:
                        blank = np.zeros(shape, dtype=np.uint8)
                        blank_shape = shape
                    self.engine.infer_array(blank)
                except Exception:                             # noqa: BLE001
                    pass

    # ----- logging -----

    def _log(self, reply: dict) -> None:
        if self._log_writer is None:
            return
        keypoints = reply.get("keypoints") or {}
        crop = reply.get("crop") or [None, None, None, None]
        position = reply.get("position") or [None, None]
        angles = reply.get("port_angles") or {}
        distances = reply.get("port_distances") or {}

        row = {
            "snapshot_id": reply.get("snapshot_id"),
            "request_time_unix": reply.get("request_time_unix"),
            "round_trip_ms": reply.get("round_trip_ms"),
            "frame_id": reply.get("frame_id"),
            "frame_sequence": reply.get("frame_sequence"),
            "capture_unix_ns": reply.get("capture_unix_ns"),
            "frame_age_ms": reply.get("frame_age_ms"),
            "inference_ms": reply.get("inference_ms"),
            "crop_x": crop[0], "crop_y": crop[1], "crop_size": crop[2],
            "ok": reply.get("ok"), "reason": reply.get("reason"),
            "heading": reply.get("heading"),
            "position_x": position[0], "position_y": position[1],
            "distance_from_centre": reply.get("distance_from_centre"),
            "angle_correction_method": reply.get("angle_correction_method"),
            "ear_distance": reply.get("ear_distance"),
            "spine_data_available": reply.get("spine_data_available"),
            "min_likelihood": reply.get("min_likelihood"),
            "left_ear_likelihood": reply.get("left_ear_likelihood"),
            "right_ear_likelihood": reply.get("right_ear_likelihood"),
        }
        for index in range(1, 7):
            row[f"port_{index}_angle"] = angles.get(index)
            row[f"port_{index}_distance"] = distances.get(index)
        for part in POSE_PARTS:
            values = keypoints.get(part)
            row[f"{part}_x"] = values[0] if values else None
            row[f"{part}_y"] = values[1] if values else None
            row[f"{part}_p"] = values[2] if values else None
        self._log_queue.put(row)

    def _log_loop(self) -> None:
        """Drain the queue onto disk, flushing about once a second.

        Writing the row inline was measured costing up to 30 ms on the occasional
        request -- a disk flush landing inside the round trip a protocol waits on.
        """
        last_flush = time.perf_counter()
        while True:
            try:
                row = self._log_queue.get(timeout=0.5)
            except queue.Empty:
                row = None
                if self._stop.is_set():
                    break
            if row is None:
                if self._log_file is not None and not self._log_file.closed:
                    self._log_file.flush()
                last_flush = time.perf_counter()
                if self._stop.is_set() and self._log_queue.empty():
                    break
                continue
            try:
                self._log_writer.writerow(row)
            except Exception:                                 # noqa: BLE001
                pass
            if time.perf_counter() - last_flush > 1.0:
                try:
                    self._log_file.flush()
                except Exception:                             # noqa: BLE001
                    pass
                last_flush = time.perf_counter()

    def status(self) -> dict:
        return {
            "ok": True,
            **self._stats(),
            "min_likelihood": self.min_likelihood,
            "ports_configured": bool(self.port_coordinates),
            "viewer_attached": self.viewer_attached,
            "gpu": self._gpu().to_dict(),
            "model": self.engine.describe(),
        }

    def close(self) -> None:
        self._stop.set()
        self._drop_reader()
        if self._log_thread is not None:
            self._log_queue.put(None)
            self._log_thread.join(timeout=3.0)
        if self._log_file is not None:
            try:
                while not self._log_queue.empty():
                    row = self._log_queue.get_nowait()
                    if row is not None:
                        self._log_writer.writerow(row)
            except Exception:                                 # noqa: BLE001
                pass
            self._log_file.flush()
            self._log_file.close()
            self._log_file = None


class _Handler(socketserver.StreamRequestHandler):
    def handle(self) -> None:
        service: PoseService = self.server.service       # type: ignore[attr-defined]
        self.connection.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        for line in self.rfile:
            line = line.strip()
            if not line:
                continue
            try:
                request = json.loads(line)
            except json.JSONDecodeError as exc:
                self._send({"ok": False, "reason": f"bad JSON: {exc}"})
                continue

            command = request.get("cmd", "snapshot")
            if command == "snapshot":
                reply = service.snapshot(max_age_ms=request.get("max_age_ms"))
            elif command == "recent":
                reply = service.recent(int(request.get("since", 0)),
                                       bool(request.get("want_image")))
            elif command == "status":
                reply = service.status()
            elif command == "ping":
                reply = {"ok": True, "pong": time.time()}
            elif command == "shutdown":
                self._send({"ok": True, "shutting_down": True})
                threading.Thread(target=self.server.shutdown, daemon=True).start()
                return
            else:
                reply = {"ok": False, "reason": f"unknown command {command!r}"}
            self._send(reply)

    def _send(self, payload: dict) -> None:
        self.wfile.write((json.dumps(payload) + "\n").encode("utf-8"))
        self.wfile.flush()


class _Server(socketserver.ThreadingTCPServer):
    allow_reuse_address = True
    daemon_threads = True


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--rig", required=True, help="matches the recorder's --rig")
    parser.add_argument("--model", required=True)
    parser.add_argument("--snapshot", default=None)
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=5801)
    parser.add_argument("--crop", type=int, default=640,
                        help="centre crop size; 0 for the whole frame")
    parser.add_argument("--crop-centre", default=None,
                        help="x,y of the scales in the frame; default is frame centre")
    parser.add_argument("--min-likelihood", type=float, default=0.6)
    parser.add_argument("--ports", default=None,
                        help="port coordinates as x,y;x,y;... in LED_1..LED_6 order")
    parser.add_argument("--arena-radius-mm", type=float, default=None,
                        help="real radius of the port circle, to report mm positions")
    parser.add_argument("--warmup-images", default=None)
    parser.add_argument("--warmup-seconds", type=float, default=3.0)
    parser.add_argument("--warmup-save", default=None)
    parser.add_argument("--log", default=None, help="CSV of every snapshot")
    parser.add_argument("--snapshot-dir", default=None,
                        help="save each snapshot as a JPEG here")
    parser.add_argument("--monitor-fps", type=float, default=2.0,
                        help="background snapshots while a viewer is open; 0 disables")
    parser.add_argument("--replay", default=None,
                        help="folder of frames to serve instead of the camera, "
                             "for testing a protocol without a rig")
    parser.add_argument("--replay-fps", type=float, default=100.0)
    parser.add_argument("--fp32", action="store_true")
    args = parser.parse_args()

    from pose_engine import PoseEngine

    port_coordinates = None
    if args.ports:
        port_coordinates = [
            tuple(float(v) for v in pair.split(","))
            for pair in args.ports.split(";") if pair.strip()
        ]
        if len(port_coordinates) != 6:
            print(f"Expected 6 port coordinates, got {len(port_coordinates)}")
            return 2

    crop_centre = None
    if args.crop_centre:
        x, y = args.crop_centre.split(",")
        crop_centre = (int(x), int(y))

    print(f"Loading {args.model}")
    started = time.perf_counter()
    try:
        engine = PoseEngine(args.model, snapshot=args.snapshot,
                            half=not args.fp32, crop_size=args.crop or None,
                            crop_centre=crop_centre)
    except Exception as exc:                                  # noqa: BLE001
        print(f"FATAL: could not load the model: {exc}")
        return 2
    info = engine.describe()
    print(f"  {info['net_type']} / {info['method']}, snapshot {info['snapshot']}, "
          f"{info['precision']}, cuda_graph={info['cuda_graph']}")
    print(f"  loaded in {time.perf_counter() - started:.1f} s")

    clocks = engine.clock_state()
    if clocks is not None and not clocks["locked"]:
        print(f"  WARNING: GPU clocks are idling "
              f"({clocks['sm_mhz']}/{clocks['sm_max_mhz']} MHz graphics, "
              f"{clocks['mem_mhz']}/{clocks['mem_max_mhz']} MHz memory).")
        print("  Occasional snapshots will cost several times what they should - "
              "up to 10x - because the GPU clocks down between them.")
        print("  Fix: run scripts/configure_rig.ps1 -Apply as Administrator.")
    elif clocks is not None:
        print(f"  GPU clocks locked ({clocks['sm_mhz']} MHz graphics, "
              f"{clocks['mem_mhz']} MHz memory)")

    # Warmup before the socket opens, so nothing can ask for a snapshot until the
    # model has been proved to work and the cold start has been paid.
    if args.warmup_images:
        from pose_warmup import run_warmup, show
        warmup = run_warmup(engine, args.warmup_images,
                            crop_size=0,          # reference mice are not centred
                            live_crop_size=args.crop or None,
                            port_coordinates=port_coordinates,
                            min_likelihood=args.min_likelihood)
        print(f"  {warmup.summary()}")
        if not warmup.ok:
            print("FATAL: the warmup failed, so the model is not usable. "
                  "Refusing to start rather than failing mid-session.")
            return 2
        show(warmup, seconds=args.warmup_seconds, save_to=args.warmup_save,
             title=f"Pose warmup - {args.rig}")
    else:
        engine.warmup(rounds=8, crop_size=args.crop or None)
        print("  warmed up (no reference images configured)")

    if args.replay:
        print(f"REPLAY MODE: serving frames from {args.replay}, not the camera")
        def source():
            return ReplayReader(args.replay, fps=args.replay_fps)
    else:
        def source():
            return FrameOutReader(args.rig)

    # With the clocks locked the GPU holds its speed on its own and the keepalive
    # is only a cheap safety net. Without, it has to do the job by working the
    # GPU, which needs to be far more frequent to achieve anything.
    keepalive_s = 0.5 if (clocks is None or clocks["locked"]) else 0.05
    if clocks is not None and not clocks["locked"]:
        print(f"  keepalive every {keepalive_s * 1000:.0f} ms to hold the clocks up; "
              f"lock them instead and this drops to 500 ms")

    # mm per pixel falls out of the port calibration that already exists: the
    # configured ports describe a circle of known pixel radius, so one real
    # measurement of that radius is the whole calibration.
    mm_per_pixel = None
    if args.arena_radius_mm and port_coordinates:
        cx = sum(p[0] for p in port_coordinates) / 6
        cy = sum(p[1] for p in port_coordinates) / 6
        radius_px = sum(math.hypot(p[0] - cx, p[1] - cy)
                        for p in port_coordinates) / 6
        mm_per_pixel = args.arena_radius_mm / radius_px
        print(f"  arena {radius_px:.0f} px radius = {args.arena_radius_mm:.0f} mm, "
              f"so {mm_per_pixel:.4f} mm/px")

    service = PoseService(
        engine,
        source,
        port_coordinates=port_coordinates,
        crop_size=args.crop or None,
        min_likelihood=args.min_likelihood,
        log_path=Path(args.log) if args.log else None,
        keepalive_s=keepalive_s,
        mm_per_pixel=mm_per_pixel,
        snapshot_dir=Path(args.snapshot_dir) if args.snapshot_dir else None,
        monitor_fps=args.monitor_fps,
    )

    # Attach now rather than on the first request. Replay mode in particular
    # reads every image off disk when it attaches, and paying that on the first
    # snapshot makes the first latency figure a lie.
    try:
        service._ensure_reader()
    except Exception as exc:                                  # noqa: BLE001
        print(f"  frame source not ready yet ({exc}); will retry per request")

    for target in (service.keepalive_loop, service.render_loop, service.monitor_loop):
        threading.Thread(target=target, daemon=True).start()

    server = _Server((args.host, args.port), _Handler)
    server.service = service                                  # type: ignore[attr-defined]
    print(f"Listening on {args.host}:{args.port} for rig {args.rig}")
    if args.log:
        print(f"Logging every snapshot to {args.log}")
    sys.stdout.flush()

    try:
        server.serve_forever(poll_interval=0.2)
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()
        service.close()
    print("Pose server stopped.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
