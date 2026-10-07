"""The live pose server: one process per rig, answering "where is the mouse looking?"

Sits between the recorder's shared memory and the behaviour system. A protocol
asks for a heading; this takes the newest frame, runs the model on it, computes
the bearing exactly as the offline analysis would, logs everything, and replies.

Inference is **on demand, not continuous**. A reading is wanted at specific
moments -- a cue decision -- not sixty times a second, so running the model
constantly would burn the GPU for nothing and make the live system a source of
dropped frames rather than a consumer of them. Between requests the server does
almost nothing.

Separate process rather than a thread in the recorder, for two reasons that both
come down to the recording being the thing that must not break: a crash anywhere
in the PyTorch or CUDA stack takes down only this, and nothing it does can stall
the capture thread.

The wire protocol is newline-delimited JSON over a loopback TCP socket. Loopback
costs about 0.1 ms, any language can speak it, and it can be driven by hand from
a terminal when something is wrong -- which matters more than a faster protocol
when a rig is misbehaving at 9pm.

    python python/pose_server.py --rig rig3 --model <path> --port 5801

Requests, one JSON object per line:

    {"cmd": "heading", "target_port": 3, "max_age_ms": 100}
    {"cmd": "status"}
    {"cmd": "shutdown"}

A heading reply carries the pose it came from, not just the answer, so a decision
can be re-derived afterwards from the recorded video.
"""

from __future__ import annotations

import argparse
import csv
import json
import queue
import socket
import socketserver
import sys
import threading
import time
from pathlib import Path
from typing import Optional

sys.path.insert(0, str(Path(__file__).resolve().parent))

from frame_out_reader import FrameOutReader, FrameOutError, NotRunning  # noqa: E402
from head_angle import REQUIRED_PARTS, angle_to_port, head_angle       # noqa: E402

# Columns of the per-inference log. One row per request, not per frame: inference
# is on demand, so the record is sparse and small.
LOG_COLUMNS = [
    "request_time_unix", "reply_time_unix", "round_trip_ms",
    "frame_id", "frame_sequence", "capture_unix_ns", "frame_age_ms",
    "inference_ms", "crop_x", "crop_y", "crop_size",
    "bearing", "midpoint_x", "midpoint_y",
    "angle_correction_method", "ear_distance", "spine_data_available",
    "min_likelihood", "left_ear_likelihood", "right_ear_likelihood",
    "target_port", "cue_presentation_angle", "ok", "reason",
]
for _part in ("left_ear", "right_ear", "spine_1", "spine_2", "spine_3", "spine_4"):
    LOG_COLUMNS += [f"{_part}_x", f"{_part}_y", f"{_part}_p"]


class ReplayReader:
    """A stand-in for the shared memory block, backed by a folder of images.

    Lets the whole server, client and protocol path be exercised with real mice
    without a rig: there is no way to put a mouse in front of a bench camera, and
    "it works except for the part that sees the animal" is not a tested system.

    Presents the same surface as FrameOutReader, so nothing downstream knows the
    difference. Frames advance on a clock at the configured rate, as a camera's
    would, so frame ages and request pacing behave realistically.
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

        self.names = [p.name for p in paths]
        self.height, self.width = self.images[0].shape[:2]
        self.stride = self.width
        self.frame_bytes = self.width * self.height
        self.qpc_frequency = 10_000_000
        self.name = f"replay:{folder}"
        self._fps = fps
        self._started = time.perf_counter()

    def latest(self):
        from frame_out_reader import Frame
        elapsed = time.perf_counter() - self._started
        index = int(elapsed * self._fps)
        image = self.images[index % len(self.images)]
        now_ns = int(time.time() * 1e9)
        return Frame(
            image=image,
            frame_id=index,
            sequence=index + 1,
            capture_qpc=0,
            capture_unix_ns=now_ns,
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
                 log_path: Optional[Path] = None, keepalive_s: float = 0.5):
        self.engine = engine
        self._reader_factory = reader_factory
        self._reader: Optional[FrameOutReader] = None
        self.port_coordinates = port_coordinates
        self.crop_size = crop_size
        self.min_likelihood = min_likelihood
        self.keepalive_s = keepalive_s

        self._lock = threading.Lock()
        self._stop = threading.Event()
        self._last_activity = time.perf_counter()
        # The shape real inferences run at. The keepalive must use exactly this:
        # a CUDA graph is captured per input shape, so a keepalive on a
        # differently sized blank would capture a second graph - 300 ms of
        # capture, holding the lock, landing on whatever request came next.
        self._last_shape: Optional[tuple[int, int]] = None

        self.requests = 0
        self.failures = 0
        self.started_unix = time.time()

        # Logging runs on its own thread behind a queue. Writing the row inline
        # was measured costing up to 30 ms on the occasional request - a disk
        # flush landing inside the round trip the protocol is waiting on. The
        # queue put is microseconds, and the writer flushes on its own clock, so
        # a crash still costs at most a second of rows.
        self._log_file = None
        self._log_writer = None
        self._log_queue: "queue.Queue[Optional[dict]]" = queue.Queue()
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

    def _ensure_reader(self) -> FrameOutReader:
        """Attach lazily, and re-attach if the recorder restarted.

        The server may well be started before the recorder has created the block,
        so a failure to attach is not fatal at startup -- it is retried on each
        request until it works.
        """
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

    # ----- the request -----

    def heading(self, *, target_port: Optional[int] = None,
                max_age_ms: Optional[float] = None) -> dict:
        request_time = time.time()
        started = time.perf_counter()

        with self._lock:
            self._last_activity = started
            self.requests += 1
            try:
                reply = self._heading_locked(target_port, max_age_ms)
            except NotRunning as exc:
                self._drop_reader()
                reply = {"ok": False, "reason": f"recorder not running: {exc}"}
            except FrameOutError as exc:
                self._drop_reader()
                reply = {"ok": False, "reason": f"frame out error: {exc}"}
            except Exception as exc:                          # noqa: BLE001
                reply = {"ok": False, "reason": f"{type(exc).__name__}: {exc}"}

        reply_time = time.time()
        reply["round_trip_ms"] = round((time.perf_counter() - started) * 1000.0, 3)
        reply["request_time_unix"] = request_time
        reply["reply_time_unix"] = reply_time
        if not reply.get("ok"):
            self.failures += 1
        self._log(reply, target_port)
        return reply

    def _heading_locked(self, target_port, max_age_ms) -> dict:
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

        result = head_angle(keypoints, port_coordinates=self.port_coordinates,
                            correct_port=target_port)
        if result is None:
            return {"ok": False, "reason": "no ears found",
                    "frame_id": frame.frame_id,
                    "inference_ms": round(self.engine.last_inference_ms, 3)}

        confident = result.min_likelihood >= self.min_likelihood

        reply = {
            "ok": confident,
            "reason": None if confident else
                      f"low confidence ({result.min_likelihood:.2f} < "
                      f"{self.min_likelihood:.2f})",
            "bearing": round(result.bearing, 3),
            "midpoint": [round(result.midpoint[0], 2), round(result.midpoint[1], 2)],
            "angle_correction_method": result.angle_correction_method,
            "ear_distance": round(result.ear_distance, 2),
            "spine_data_available": result.spine_data_available,
            "min_likelihood": round(result.min_likelihood, 4),
            "left_ear_likelihood": round(result.left_ear_likelihood, 4),
            "right_ear_likelihood": round(result.right_ear_likelihood, 4),
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
            reply["crop"] = [crop.x, crop.y, crop.width, crop.height]
        if self.port_coordinates:
            reply["relative_angles"] = [round(a, 3) for a in result.relative_angles]
        if target_port is not None:
            reply["target_port"] = target_port
            signed = angle_to_port(result, target_port)
            reply["cue_presentation_angle"] = \
                None if signed is None else round(signed, 3)
        return reply

    # ----- logging -----

    def _log(self, reply: dict, target_port) -> None:
        if self._log_writer is None:
            return
        keypoints = reply.get("keypoints") or {}
        crop = reply.get("crop") or [None, None, None, None]
        row = {
            "request_time_unix": reply.get("request_time_unix"),
            "reply_time_unix": reply.get("reply_time_unix"),
            "round_trip_ms": reply.get("round_trip_ms"),
            "frame_id": reply.get("frame_id"),
            "frame_sequence": reply.get("frame_sequence"),
            "capture_unix_ns": reply.get("capture_unix_ns"),
            "frame_age_ms": reply.get("frame_age_ms"),
            "inference_ms": reply.get("inference_ms"),
            "crop_x": crop[0], "crop_y": crop[1], "crop_size": crop[2],
            "bearing": reply.get("bearing"),
            "midpoint_x": (reply.get("midpoint") or [None, None])[0],
            "midpoint_y": (reply.get("midpoint") or [None, None])[1],
            "angle_correction_method": reply.get("angle_correction_method"),
            "ear_distance": reply.get("ear_distance"),
            "spine_data_available": reply.get("spine_data_available"),
            "min_likelihood": reply.get("min_likelihood"),
            "left_ear_likelihood": reply.get("left_ear_likelihood"),
            "right_ear_likelihood": reply.get("right_ear_likelihood"),
            "target_port": target_port,
            "cue_presentation_angle": reply.get("cue_presentation_angle"),
            "ok": reply.get("ok"),
            "reason": reply.get("reason"),
        }
        for part in ("left_ear", "right_ear", "spine_1", "spine_2", "spine_3", "spine_4"):
            values = keypoints.get(part)
            row[f"{part}_x"] = values[0] if values else None
            row[f"{part}_y"] = values[1] if values else None
            row[f"{part}_p"] = values[2] if values else None
        self._log_queue.put(row)

    def _log_loop(self) -> None:
        """Drain the queue onto disk, flushing about once a second."""
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

    # ----- keepalive -----

    def keepalive_loop(self) -> None:
        """A throwaway inference now and then, so the GPU does not clock down.

        This is a fallback, not the fix. An idle GPU drops its graphics clock to
        a few hundred MHz and its memory clock with it, and an inference once a
        second then costs about ten times one run back to back -- measured at
        8 ms against 94 ms. The inter-trial interval is exactly such a gap, so
        the penalty would land on the request that matters most.

        The real fix is locking both clocks, which makes latency flat at any
        request rate for about 15 W: run ``scripts/configure_rig.ps1 -Apply``.
        That needs administrator, so this process cannot do it, and when the
        clocks are not locked it falls back to keeping the GPU awake by working
        it. Measured, that needs an inference every 50 ms or so to be worth
        anything -- a 0.5 s keepalive does nothing useful -- which is a real
        slice of the GPU spent on nothing. Hence: cheap when the clocks are
        locked, expensive only when it has to be.
        """
        import numpy as np
        blank = None
        blank_shape = None
        while not self._stop.wait(self.keepalive_s):
            if time.perf_counter() - self._last_activity < self.keepalive_s:
                continue
            shape = self._last_shape
            if shape is None:
                # Nothing real has run yet, so the shape to keep warm is not
                # known. Guessing would capture a graph that is never used.
                continue
            with self._lock:
                try:
                    if blank is None or blank_shape != shape:
                        blank = np.zeros(shape, dtype=np.uint8)
                        blank_shape = shape
                    self.engine.infer_array(blank)
                except Exception:                             # noqa: BLE001
                    pass

    def status(self) -> dict:
        info = self.engine.describe()
        attached = self._reader is not None
        return {
            "ok": True,
            "uptime_s": round(time.time() - self.started_unix, 1),
            "requests": self.requests,
            "failures": self.failures,
            "frame_out_attached": attached,
            "crop_size": self.crop_size,
            "min_likelihood": self.min_likelihood,
            "ports_configured": bool(self.port_coordinates),
            "gpu_clocks": self.engine.clock_state(),
            "model": info,
        }

    def close(self) -> None:
        self._stop.set()
        self._drop_reader()
        if self._log_thread is not None:
            self._log_queue.put(None)          # wake it so it can finish and exit
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

            command = request.get("cmd", "heading")
            if command == "heading":
                reply = service.heading(
                    target_port=request.get("target_port"),
                    max_age_ms=request.get("max_age_ms"))
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
    parser.add_argument("--warmup-images", default=None)
    parser.add_argument("--warmup-seconds", type=float, default=3.0)
    parser.add_argument("--warmup-save", default=None)
    parser.add_argument("--log", default=None, help="CSV of every inference")
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
        print(f"  Occasional inferences will cost several times what they should "
              f"- up to 10x - because the GPU clocks down between them and is slow "
              f"to come back.")
        print(f"  Fix: run scripts/configure_rig.ps1 -Apply as Administrator.")
    elif clocks is not None:
        print(f"  GPU clocks locked ({clocks['sm_mhz']} MHz graphics, "
              f"{clocks['mem_mhz']} MHz memory)")

    # Warmup before the socket opens, so nothing can ask for a heading until the
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
        source = lambda: ReplayReader(args.replay, fps=args.replay_fps)
    else:
        source = lambda: FrameOutReader(args.rig)

    # With the clocks locked the GPU holds its speed on its own and the
    # keepalive is only a cheap safety net. Without, it has to do the job by
    # working the GPU, which needs to be far more frequent to achieve anything.
    keepalive_s = 0.5 if (clocks is None or clocks["locked"]) else 0.05
    if clocks is not None and not clocks["locked"]:
        print(f"  keepalive every {keepalive_s * 1000:.0f} ms to hold the clocks up; "
              f"lock them instead and this drops to {500:.0f} ms")

    service = PoseService(
        engine,
        source,
        port_coordinates=port_coordinates,
        crop_size=args.crop or None,
        min_likelihood=args.min_likelihood,
        log_path=Path(args.log) if args.log else None,
        keepalive_s=keepalive_s,
    )
    # Attach now rather than on the first request. Replay mode in particular
    # reads every image off disk when it attaches, and paying that on the first
    # heading makes the first latency figure a lie.
    try:
        service._ensure_reader()
    except Exception as exc:                                  # noqa: BLE001
        print(f"  frame source not ready yet ({exc}); will retry per request")

    threading.Thread(target=service.keepalive_loop, daemon=True).start()

    server = _Server((args.host, args.port), _Handler)
    server.service = service                                  # type: ignore[attr-defined]
    print(f"Listening on {args.host}:{args.port} for rig {args.rig}")
    if args.log:
        print(f"Logging every inference to {args.log}")
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
