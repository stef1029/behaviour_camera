"""Pull frames out of recorded sessions, to benchmark and sanity-check pose on.

Live pose has to be measured on real footage, not on a synthetic test pattern: the
arena lighting, the mouse's size in frame and the amount of sensor noise all change
how long inference takes and how well it works. This grabs a spread of frames from
across several sessions and writes them to a folder as PNGs.

    python scripts/extract_benchmark_frames.py --count 60

Frames are taken from well inside each video rather than near the start, because
the first and last stretches of a session often have no mouse in them. Written as
PNG rather than JPEG so the benchmark is not measuring compression artefacts.
"""

from __future__ import annotations

import argparse
import random
import sys
from pathlib import Path

COHORT = Path(r"Y:\dwelch\Behaviour\2604_audiospatial")
VIDEO_SUFFIXES = (".avi", ".mkv", ".mp4")

# Videos the post-processing produced rather than the camera, which must not be
# benchmarked on - they carry overlays and a different resolution.
DERIVED_MARKERS = ("labeled", "labelled", "_dlc", "cropped", "overlay")


def find_session_videos(cohort: Path) -> list[Path]:
    found = []
    for path in sorted(cohort.rglob("*")):
        if path.suffix.lower() not in VIDEO_SUFFIXES:
            continue
        lowered = path.name.lower()
        if any(marker in lowered for marker in DERIVED_MARKERS):
            continue
        found.append(path)
    return found


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cohort", default=str(COHORT))
    parser.add_argument("--out", default="benchmark_frames")
    parser.add_argument("--count", type=int, default=60, help="total frames to write")
    parser.add_argument("--videos", type=int, default=6, help="how many videos to sample")
    parser.add_argument("--seed", type=int, default=20261007)
    args = parser.parse_args()

    try:
        import cv2
    except ImportError:
        print("OpenCV is needed to read the videos: pip install opencv-python")
        return 2

    cohort = Path(args.cohort)
    if not cohort.exists():
        print(f"Cohort not reachable: {cohort}")
        return 2

    videos = find_session_videos(cohort)
    if not videos:
        print(f"No session videos under {cohort}")
        return 2
    print(f"{len(videos)} session videos found")

    rng = random.Random(args.seed)
    chosen = rng.sample(videos, min(args.videos, len(videos)))
    per_video = max(1, args.count // len(chosen))

    out_dir = Path(args.out)
    out_dir.mkdir(parents=True, exist_ok=True)

    written = 0
    for video_path in chosen:
        capture = cv2.VideoCapture(str(video_path))
        if not capture.isOpened():
            print(f"  could not open {video_path.name}")
            continue

        total = int(capture.get(cv2.CAP_PROP_FRAME_COUNT))
        width = int(capture.get(cv2.CAP_PROP_FRAME_WIDTH))
        height = int(capture.get(cv2.CAP_PROP_FRAME_HEIGHT))

        if total <= 0:
            # Some containers will not report a frame count. Walking the whole
            # file to find out is not worth it; skip and use another video.
            print(f"  {video_path.name}: no frame count reported, skipping")
            capture.release()
            continue

        # The middle 60% of the session, where there is reliably a mouse.
        low, high = int(total * 0.2), int(total * 0.8)
        if high <= low:
            low, high = 0, max(1, total - 1)

        print(f"  {video_path.name}: {width}x{height}, {total} frames")
        for _ in range(per_video):
            index = rng.randint(low, high)
            capture.set(cv2.CAP_PROP_POS_FRAMES, index)
            ok, frame = capture.read()
            if not ok or frame is None:
                continue
            # The recorder produces Mono8, so benchmark on single channel.
            if frame.ndim == 3:
                frame = cv2.cvtColor(frame, cv2.COLOR_BGR2GRAY)
            name = out_dir / f"{video_path.stem}_f{index:06d}.png"
            if cv2.imwrite(str(name), frame):
                written += 1
        capture.release()

    print()
    print(f"{written} frames written to {out_dir.resolve()}")
    if written == 0:
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
