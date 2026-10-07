"""Check the frame-out shared memory block, and save what came through it.

Attaches to a running recorder, reads frames for a few seconds, and reports
whether what arrived makes sense: frame IDs advancing, no torn reads, read
latency well under a frame period. Saves a handful of frames as PNGs so there is
something to look at without having to run the GUI.

    python scripts/test_frame_out.py --rig bench --seconds 5

The recorder must be running with --frame-out. Nothing here writes to the block,
and nothing here can affect the recording.

Exit codes: 0 all good, 1 attached but something looks wrong, 2 could not attach.
"""

from __future__ import annotations

import argparse
import statistics
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "python"))

from frame_out_reader import FrameOutReader, FrameOutError, NotRunning  # noqa: E402


def save_png(path: Path, image) -> bool:
    """Write a greyscale PNG. Prefers imageio, falls back to OpenCV, then skips."""
    try:
        import imageio.v3 as iio
        iio.imwrite(path, image)
        return True
    except Exception:
        pass
    try:
        import cv2
        return bool(cv2.imwrite(str(path), image))
    except Exception:
        return False


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--rig", default="bench", help="rig name the recorder was given")
    parser.add_argument("--seconds", type=float, default=5.0)
    parser.add_argument("--save", type=int, default=3, help="how many frames to save as PNG")
    parser.add_argument("--out", default="screenshots_temp",
                        help="where to put the saved frames")
    args = parser.parse_args()

    try:
        reader = FrameOutReader(args.rig)
    except NotRunning as exc:
        print(f"Not running: {exc}")
        return 2
    except FrameOutError as exc:
        print(f"Could not attach: {exc}")
        return 2

    print(f"Attached to {reader.name}")
    print(f"  image        {reader.width}x{reader.height}, stride {reader.stride}")
    print(f"  frame slot   {reader.frame_bytes} bytes x 2")
    print(f"  QPC          {reader.qpc_frequency} Hz")
    print()

    out_dir = Path(args.out)
    out_dir.mkdir(parents=True, exist_ok=True)

    # Two different numbers, and conflating them is misleading. wait_ms is how
    # long until the *next* frame exists, which at 100 fps is about 10 ms and is
    # just the frame period. copy_ms is how long taking a frame that already
    # exists costs -- that is the one that lands in the live-pose latency budget,
    # because a pose request takes the newest frame rather than waiting for one.
    wait_ms: list[float] = []
    copy_ms: list[float] = []
    gaps: list[int] = []
    saved = 0
    count = 0
    timeouts = 0
    first_id = None
    last_id = None
    torn = 0

    deadline = time.perf_counter() + args.seconds
    while time.perf_counter() < deadline:
        started = time.perf_counter()
        try:
            frame = reader.wait_for_new(timeout_s=0.5)
        except NotRunning:
            print("Recorder stopped while reading.")
            break
        except FrameOutError as exc:
            # The reader raises this only after repeated torn reads.
            print(f"Read failure: {exc}")
            torn += 1
            break

        if frame is None:
            timeouts += 1
            continue

        wait_ms.append((time.perf_counter() - started) * 1000.0)
        count += 1

        # Time a read of the frame already sitting there, which is what a pose
        # request actually does.
        copy_started = time.perf_counter()
        reader.latest()
        copy_ms.append((time.perf_counter() - copy_started) * 1000.0)

        if last_id is not None:
            gaps.append(frame.frame_id - last_id)
        else:
            first_id = frame.frame_id
        last_id = frame.frame_id

        if saved < args.save:
            name = out_dir / f"frame_out_{saved:02d}_id{frame.frame_id}.png"
            if save_png(name, frame.image):
                mean = float(frame.image.mean())
                print(f"  saved {name.name}  mean pixel {mean:.1f}")
            else:
                print(f"  could not save {name.name} (no imageio or cv2 available)")
            saved += 1

    reader_frames_captured = None
    try:
        latest = reader.latest()
        if latest is not None:
            reader_frames_captured = latest.frames_captured
    except FrameOutError:
        pass
    reader.close()

    print()
    print(f"frames read            {count}")
    if first_id is not None and last_id is not None:
        print(f"frame IDs              {first_id} .. {last_id}")
    if reader_frames_captured is not None:
        print(f"recorder captured      {reader_frames_captured}")
    print(f"empty polls (timeouts) {timeouts}")

    problems = []

    if count == 0:
        problems.append("no frames arrived at all")
    else:
        print(f"wait for next ms      median {statistics.median(wait_ms):.2f}  "
              f"max {max(wait_ms):.2f}   (~one frame period; expected)")
        print(f"copy newest ms        median {statistics.median(copy_ms):.2f}  "
              f"max {max(copy_ms):.2f}   (the latency-budget figure)")

    if gaps:
        # The reader is not meant to see every frame -- it reads the newest one,
        # so skipping is normal and expected. What must never happen is a gap of
        # zero or less, which would mean the same frame twice or going backwards.
        backwards = [g for g in gaps if g <= 0]
        print(f"frame ID steps         median {statistics.median(gaps)}  "
              f"min {min(gaps)}  max {max(gaps)}")
        if backwards:
            problems.append(f"{len(backwards)} frame IDs did not advance "
                            f"(a torn or stale read got through)")

    if torn:
        problems.append("the reader gave up after repeated torn reads")

    print()
    if problems:
        for problem in problems:
            print(f"PROBLEM: {problem}")
        return 1

    print("Frame out looks healthy.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
