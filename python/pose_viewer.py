"""The per-rig DLC live window: what the model is seeing, while it runs.

A standalone DearPyGui process, one per rig, following the pattern hexcontrol
already uses for its DAQ view. It needs nothing from the pose stack but a socket:
no torch, no CUDA, no DeepLabCut. It runs in hexcontrol's own environment and
talks to the server over the same loopback port a protocol uses.

**It shows only what was derived from the image.** Heading, position, the angle
and distance to every port, how confident the model was, and how long it took.
It does not know and cannot show which port a protocol went on to cue, or what
the animal did next -- the pose system is a perception service and deliberately
has no channel for that. What is on screen is what the camera saw.

    python python/pose_viewer.py --rig "Rig 3" --port 5803

Polls ``recent`` a few times a second. That poll is also what tells the server a
viewer is attached: with no window open it does no drawing, no encoding and no
background snapshots at all.
"""

from __future__ import annotations

import argparse
import base64
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from pose_client import PoseClient  # noqa: E402

POLL_HZ = 6.0
PORTS = (1, 2, 3, 4, 5, 6)

# DearPyGui dark theme, RGBA.
TEXT = (220, 220, 220)
DIM = (145, 145, 150)
GOOD = (90, 200, 90)
WARN = (250, 180, 60)
BAD = (235, 70, 70)
ACCENT = (29, 151, 236)


def _fmt(value, spec="{:.1f}", dash="-"):
    return dash if value is None else spec.format(value)


class Viewer:
    def __init__(self, rig: str, host: str, port: int, image_size: int = 460):
        self.rig = rig
        self.client = PoseClient(host, port)
        self.image_size = image_size
        self.since = 0
        self.latest = None
        self.history: list[dict] = []
        self.gpu = {}
        self.stats = {}
        self.connected = False
        self.last_error = ""
        self._image_id = -1
        self._rows_built = 0

    # ----- polling -----

    def poll(self) -> None:
        reply = self.client.recent(self.since, want_image=True, timeout_ms=2000)
        if not reply.get("ok"):
            self.connected = False
            self.last_error = reply.get("reason") or "no reply"
            return
        self.connected = True
        self.last_error = ""

        snapshots = reply.get("snapshots") or []
        if snapshots:
            self.since = snapshots[-1]["snapshot_id"]
            self.history = (snapshots + self.history)[:40] \
                if not self.history else (list(reversed(snapshots)) + self.history)[:40]
            # Keep the newest that actually carries a reading, so the panel does
            # not blank out every time a monitor frame misses.
            for snap in reversed(snapshots):
                if snap.get("heading") is not None:
                    self.latest = snap
                    break
            else:
                self.latest = snapshots[-1]

        self.gpu = reply.get("gpu") or {}
        self.stats = reply.get("stats") or {}

        encoded = reply.get("image_jpeg_b64")
        if encoded and reply.get("image_id") != self._image_id:
            self._image_id = reply.get("image_id")
            self._set_image(base64.b64decode(encoded))

    def _set_image(self, jpeg: bytes) -> None:
        import dearpygui.dearpygui as dpg
        import cv2
        import numpy as np

        buffer = np.frombuffer(jpeg, dtype=np.uint8)
        image = cv2.imdecode(buffer, cv2.IMREAD_COLOR)
        if image is None:
            return
        image = cv2.resize(image, (self.image_size, self.image_size),
                           interpolation=cv2.INTER_AREA)
        rgba = cv2.cvtColor(image, cv2.COLOR_BGR2RGBA).astype(np.float32) / 255.0
        dpg.set_value("frame_texture", rgba.ravel())

    # ----- drawing -----

    def refresh(self) -> None:
        import dearpygui.dearpygui as dpg

        if not self.connected:
            dpg.set_value("status_text", f"no server on port {self.client.port}")
            dpg.configure_item("status_text", color=BAD)
            dpg.set_value("detail_text", self.last_error)
            return

        snap = self.latest or {}
        ok = bool(snap.get("ok"))
        heading = snap.get("heading")

        if heading is None:
            dpg.set_value("status_text", "NO READING")
            dpg.configure_item("status_text", color=BAD)
            dpg.set_value("detail_text", snap.get("reason") or "")
        else:
            dpg.set_value("status_text", f"{heading:.1f} deg")
            dpg.configure_item("status_text", color=GOOD if ok else WARN)
            method = snap.get("angle_correction_method") or ""
            note = "" if ok else f"   {snap.get('reason')}"
            dpg.set_value("detail_text", f"method {method}{note}")

        position = snap.get("position") or [None, None]
        dpg.set_value("pos_text",
                      f"x {_fmt(position[0], '{:.0f}')}   y {_fmt(position[1], '{:.0f}')}  px")
        dpg.set_value("centre_text",
                      f"{_fmt(snap.get('distance_from_centre'), '{:.0f}')} px from arena centre")

        angles = {int(k): v for k, v in (snap.get("port_angles") or {}).items()}
        distances = {int(k): v for k, v in (snap.get("port_distances") or {}).items()}
        closest = min(distances, key=distances.get) if distances else None
        ahead = min(angles, key=lambda p: abs(angles[p])) if angles else None

        for port in PORTS:
            angle = angles.get(port)
            dpg.set_value(f"port_{port}_angle", _fmt(angle, "{:+.1f} deg"))
            dpg.set_value(f"port_{port}_dist", _fmt(distances.get(port), "{:.0f} px"))
            tag = ""
            colour = TEXT
            if angle is not None and abs(angle) <= 30:
                colour = GOOD
            if port == ahead:
                tag = "ahead"
            if port == closest:
                tag = (tag + " / closest").strip(" /") if tag else "closest"
            dpg.set_value(f"port_{port}_tag", tag)
            dpg.configure_item(f"port_{port}_angle", color=colour)

        dpg.set_value("quality_text",
                      f"worst keypoint   p = {_fmt(snap.get('min_likelihood'), '{:.2f}')}")
        worst = snap.get("min_likelihood")
        dpg.configure_item("quality_text",
                           color=GOOD if (worst or 0) >= 0.6 else BAD)
        dpg.set_value("timing_text",
                      f"inference {_fmt(snap.get('inference_ms'))} ms     "
                      f"frame age {_fmt(snap.get('frame_age_ms'))} ms     "
                      f"frame {snap.get('frame_id')}")
        dpg.set_value("source_text",
                      f"#{snap.get('snapshot_id')}  {snap.get('source', '')}")

        self._refresh_history()
        self._refresh_health()

    def _refresh_history(self) -> None:
        import dearpygui.dearpygui as dpg

        rows = [r for r in self.history if r.get("source") == "request"][:8]
        for index in range(8):
            if index < len(rows):
                row = rows[index]
                stamp = time.strftime("%H:%M:%S", time.localtime(row.get("time_unix") or 0))
                angles = {int(k): v for k, v in (row.get("port_angles") or {}).items()}
                ahead = min(angles, key=lambda p: abs(angles[p])) if angles else None
                values = [
                    str(row.get("snapshot_id")), stamp, str(row.get("frame_id")),
                    _fmt(row.get("heading"), "{:.1f}"),
                    "-" if ahead is None else f"{ahead} ({angles[ahead]:+.0f})",
                    row.get("angle_correction_method") or (row.get("reason") or "-"),
                    _fmt(row.get("min_likelihood"), "{:.2f}"),
                    _fmt(row.get("round_trip_ms"), "{:.1f}"),
                ]
                colour = TEXT if row.get("ok") else BAD
            else:
                values = ["", "", "", "", "", "", "", ""]
                colour = TEXT
            for column, value in enumerate(values):
                dpg.set_value(f"hist_{index}_{column}", value)
                dpg.configure_item(f"hist_{index}_{column}", color=colour)

    def _refresh_health(self) -> None:
        import dearpygui.dearpygui as dpg

        gpu = self.gpu
        if not gpu.get("available"):
            dpg.set_value("gpu_text", f"GPU stats unavailable: {gpu.get('error') or ''}")
            dpg.configure_item("gpu_text", color=DIM)
            return

        throttle = gpu.get("throttle_reasons") or []
        sm_locked = gpu.get("sm_locked")
        mem_locked = gpu.get("mem_locked")

        dpg.set_value("gpu_text",
                      f"GPU {gpu.get('util_percent')}%     "
                      f"VRAM {gpu.get('vram_used_gb')}/{gpu.get('vram_total_gb')} GB     "
                      f"{gpu.get('temperature_c')} C     "
                      f"{gpu.get('power_w')}/{gpu.get('power_limit_w')} W")
        dpg.configure_item("gpu_text", color=TEXT)

        dpg.set_value("clock_text",
                      f"SM {gpu.get('sm_mhz')} MHz {'locked' if sm_locked else 'IDLING'}"
                      f"     MEM {gpu.get('mem_mhz')} MHz "
                      f"{'locked' if mem_locked else 'IDLING'}"
                      f"     throttle {', '.join(throttle) if throttle else 'none'}")
        dpg.configure_item("clock_text",
                           color=GOOD if (sm_locked and mem_locked and not throttle)
                           else BAD)

        stats = self.stats
        dpg.set_value("pose_text",
                      f"NVENC {gpu.get('encoder_sessions')} session(s) "
                      f"{gpu.get('encoder_fps')} fps     "
                      f"pose {stats.get('median_round_trip_ms')} ms median     "
                      f"{stats.get('requests')} requests, "
                      f"{stats.get('found_percent')}% found")


def build_ui(viewer: Viewer) -> None:
    import dearpygui.dearpygui as dpg

    dpg.create_context()
    size = viewer.image_size
    with dpg.texture_registry():
        dpg.add_raw_texture(size, size, [0.1] * (size * size * 4),
                            format=dpg.mvFormat_Float_rgba, tag="frame_texture")

    with dpg.window(tag="root"):
        dpg.add_text(f"{viewer.rig}  -  DLC Live", color=TEXT)
        dpg.add_separator()

        with dpg.group(horizontal=True):
            with dpg.group():
                dpg.add_image("frame_texture")
                dpg.add_text("what the model sees - the crop fed to the network",
                             color=DIM)
                dpg.add_text("", tag="source_text", color=DIM)

            with dpg.group():
                dpg.add_text("HEAD ANGLE", color=DIM)
                dpg.add_text("-", tag="status_text", color=GOOD)
                dpg.add_text("", tag="detail_text", color=DIM)
                dpg.add_spacer(height=6)

                dpg.add_text("POSITION IN RIG", color=DIM)
                dpg.add_text("-", tag="pos_text")
                dpg.add_text("-", tag="centre_text", color=DIM)
                dpg.add_spacer(height=6)

                dpg.add_text("ANGLE AND DISTANCE TO EACH PORT", color=DIM)
                with dpg.table(header_row=True, borders_innerH=False,
                               borders_outerH=False, borders_innerV=False,
                               borders_outerV=False, policy=dpg.mvTable_SizingFixedFit):
                    dpg.add_table_column(label="port", width_fixed=True, init_width_or_weight=50)
                    dpg.add_table_column(label="angle", width_fixed=True, init_width_or_weight=95)
                    dpg.add_table_column(label="distance", width_fixed=True, init_width_or_weight=85)
                    dpg.add_table_column(label="", width_fixed=True, init_width_or_weight=120)
                    for port in PORTS:
                        with dpg.table_row():
                            dpg.add_text(str(port))
                            dpg.add_text("-", tag=f"port_{port}_angle")
                            dpg.add_text("-", tag=f"port_{port}_dist", color=DIM)
                            dpg.add_text("", tag=f"port_{port}_tag", color=ACCENT)

                dpg.add_spacer(height=6)
                dpg.add_text("QUALITY", color=DIM)
                dpg.add_text("-", tag="quality_text")
                dpg.add_text("-", tag="timing_text", color=DIM)

        dpg.add_separator()
        dpg.add_text("SNAPSHOTS THE PROTOCOL ASKED FOR", color=DIM)
        headers = ["#", "time", "frame", "heading", "port ahead",
                   "method", "p", "ms"]
        widths = [55, 85, 80, 80, 110, 190, 55, 55]
        with dpg.table(header_row=True, borders_innerH=False, borders_outerH=False,
                       borders_innerV=False, borders_outerV=False,
                       policy=dpg.mvTable_SizingFixedFit):
            for header, width in zip(headers, widths):
                dpg.add_table_column(label=header, width_fixed=True,
                                     init_width_or_weight=width)
            for index in range(8):
                with dpg.table_row():
                    for column in range(len(headers)):
                        dpg.add_text("", tag=f"hist_{index}_{column}")

        dpg.add_separator()
        dpg.add_text("-", tag="gpu_text", color=TEXT)
        dpg.add_text("-", tag="clock_text", color=GOOD)
        dpg.add_text("-", tag="pose_text", color=DIM)

    dpg.create_viewport(title=f"{viewer.rig} - DLC Live", width=1180, height=960)
    dpg.setup_dearpygui()
    dpg.show_viewport()
    dpg.set_primary_window("root", True)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--rig", default="Rig")
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=5801)
    parser.add_argument("--image-size", type=int, default=460)
    args = parser.parse_args()

    try:
        import dearpygui.dearpygui as dpg
    except ImportError:
        print("dearpygui is needed for the viewer. Run it in hexcontrol's "
              "environment, which already has it.")
        return 2

    viewer = Viewer(args.rig, args.host, args.port, args.image_size)
    build_ui(viewer)

    period = 1.0 / POLL_HZ
    next_poll = 0.0
    while dpg.is_dearpygui_running():
        now = time.perf_counter()
        if now >= next_poll:
            next_poll = now + period
            try:
                viewer.poll()
                viewer.refresh()
            except Exception as exc:                          # noqa: BLE001
                # A viewer must never be the thing that stops a session, so a
                # failure here is shown and ignored rather than raised.
                viewer.connected = False
                viewer.last_error = f"{type(exc).__name__}: {exc}"
        dpg.render_dearpygui_frame()

    dpg.destroy_context()
    viewer.client.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
