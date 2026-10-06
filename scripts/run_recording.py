"""Run a real recording.

Edit the settings below, then run this file (F5 in VS Code, or `python
scripts/run_recording.py`). It calls the installed behaviour_camera.exe, records to
the path you choose, and checks the result afterwards.

Nothing here needs a virtualenv - standard library only.
"""

from __future__ import annotations

import json
import subprocess
import sys
import time
from datetime import datetime
from pathlib import Path

# ============================================================================
# SETTINGS - edit these
# ============================================================================

SERIAL_NUMBER = "26043809"      # which camera; camera_probe lists the ones attached
MOUSE_ID = "test"               # goes into every filename in the session folder
OUTPUT_ROOT = r"E:\test_vid_output"   # a session folder is created inside this
FPS = 60.0                      # clamped to what the camera allows

# How frames are stored. None uses whatever the config file says (currently "raw").
#   "raw"    every frame to a .bin - biggest files, nothing touched
#   "video"  encoded on the GPU - vastly smaller, and far less written to disk
RECORDING_MODE: str | None = None

WINDOW_WIDTH = 800              # preview window
WINDOW_HEIGHT = 600

# How to stop. None means record until you press Esc in the preview window (the
# normal way to run a session). A number of seconds makes it stop itself, which is
# useful for a quick check that everything works.
DURATION_SECONDS: float | None = None

VERIFY_AFTERWARDS = True        # run check_recording.py on the result
USE_INSTALLED_BUILD = True      # False runs straight from the build tree instead

# ============================================================================


REPO = Path(__file__).resolve().parent.parent
INSTALL_EXE = REPO / "out" / "install" / "ninja-release" / "bin" / "behaviour_camera.exe"
BUILD_EXE = REPO / "out" / "build" / "ninja-release" / "behaviour_camera.exe"

def rig_name(session_dir: Path, timeout: float = 15.0) -> str | None:
    """The rig name this session is recording under, or None if it never appeared.

    Read from the metadata the recorder writes as soon as it starts, rather than
    worked out here. The rig name comes from the config file, so duplicating that
    lookup would mean two places to keep in step, and the stop-signal filename
    depends on getting it right.
    """
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        for meta in session_dir.glob("*_Tracker_data.json"):
            try:
                rig = json.loads(meta.read_text()).get("rig")
            except (OSError, json.JSONDecodeError):
                continue    # still being written; try again shortly
            if rig:
                return rig
        time.sleep(0.2)
    return None


def find_executable() -> Path:
    """The installed build if asked for and present, otherwise the build tree."""
    if USE_INSTALLED_BUILD:
        if INSTALL_EXE.exists():
            return INSTALL_EXE
        print(f"No installed build at {INSTALL_EXE}")
        if BUILD_EXE.exists():
            print("Falling back to the build tree. To install:")
            print(f"  cd {REPO}")
            print("  build.cmd")
            print("  cmake --install out/build/ninja-release")
            print()
            return BUILD_EXE
    elif BUILD_EXE.exists():
        return BUILD_EXE

    print("Could not find behaviour_camera.exe. Build it first:")
    print(f"  cd {REPO}")
    print("  build.cmd")
    sys.exit(1)


def main() -> int:
    # The recorder and the checker write straight to the console while this
    # script's own prints sit in a buffer, so without this their output arrives
    # out of order and the summary appears above the thing it summarises.
    sys.stdout.reconfigure(line_buffering=True)

    exe = find_executable()

    # The recorder names its files {date}_{id}_*, so the session folder is given the
    # same stamp and the two agree.
    stamp = datetime.now().strftime("%y%m%d_%H%M%S")
    session_dir = Path(OUTPUT_ROOT) / f"{stamp}_{MOUSE_ID}"

    try:
        session_dir.mkdir(parents=True, exist_ok=True)
    except OSError as exc:
        print(f"Could not create {session_dir}: {exc}")
        print("Check that the drive exists and is writable.")
        return 2

    command = [
        str(exe),
        "--serial_number", SERIAL_NUMBER,
        "--id", MOUSE_ID,
        "--date", stamp,
        "--path", str(session_dir),
        "--fps", str(FPS),
        "--windowWidth", str(WINDOW_WIDTH),
        "--windowHeight", str(WINDOW_HEIGHT),
    ]
    if RECORDING_MODE:
        command += ["--mode", RECORDING_MODE]

    print(f"Camera    : {SERIAL_NUMBER} at {FPS:g} fps")
    print(f"Recording : {session_dir}")
    print(f"Using     : {exe}")
    print(f"Mode      : {RECORDING_MODE or 'from config'}")
    # 1280x1024 Mono8 is 1.25 MiB a frame. Only meaningful for raw: encoding cuts
    # the write rate by roughly a hundredfold, so quoting it would be misleading.
    if RECORDING_MODE != "video":
        print(f"Disk rate : up to {1.25 * FPS:.0f} MiB/s, {1.25 * FPS * 3.6:.0f} GB/hour if raw")
    print()

    if DURATION_SECONDS is None:
        print("Press Esc in the preview window to stop.")
        print()
        process = subprocess.run(command)
        exit_code = process.returncode
    else:
        print(f"Stopping automatically after {DURATION_SECONDS:g} s "
              f"(or press Esc in the preview window).")
        print()
        process = subprocess.Popen(command)

        # Let it fail loudly - bad serial, camera already open elsewhere - before
        # starting the clock on a wait that would otherwise be pointless.
        time.sleep(2)
        if process.poll() is not None:
            print(f"\nThe recorder exited immediately with code {process.returncode}.")
            return 2

        try:
            process.wait(timeout=max(0.0, DURATION_SECONDS - 2))
        except subprocess.TimeoutExpired:
            rig = rig_name(session_dir)
            if rig is None:
                print("Could not work out the rig name; press Esc in the preview window.")
                process.wait()
            else:
                (session_dir / f"stop_camera_{rig}.signal").touch()
            try:
                process.wait(timeout=30)
            except subprocess.TimeoutExpired:
                print("The recorder did not stop within 30 s; terminating it.")
                print("The files it wrote may be incomplete.")
                process.kill()
                process.wait(timeout=5)
        exit_code = process.returncode

    print()
    print(f"Recorder exited with code {exit_code}.")

    files = sorted(session_dir.iterdir())
    if files:
        print(f"\nFiles in {session_dir}:")
        for f in files:
            print(f"  {f.name:<55}{f.stat().st_size / 1048576:>9.1f} MiB")

    if VERIFY_AFTERWARDS:
        print()
        checker = Path(__file__).resolve().parent / "check_recording.py"
        result = subprocess.run([sys.executable, str(checker), str(session_dir)])
        # A non-zero result here means the recording is suspect even if the
        # recorder itself exited cleanly, so it decides the overall outcome.
        return result.returncode or (1 if exit_code != 0 else 0)

    return 1 if exit_code != 0 else 0


if __name__ == "__main__":
    sys.exit(main())
