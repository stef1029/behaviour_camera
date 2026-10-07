"""How long does live pose actually take, and what does cropping cost in accuracy?

The whole live-pose plan rests on one number: inference time for this model on
this GPU. This measures it rather than assuming it, and reports both halves of
the trade-off -- the time, and whether the keypoints the heading calculation needs
are still found. A crop that is fast but clips the mouse's spine is not a saving,
because the spine is what catches an ear swap.

Two things about the method, because a careless version of this would mislead.

**Crops are centred on the mouse, not on the frame.** A reading is only taken
while the mouse is on the scales, so in use it will be near the middle of the
crop. The benchmark frames are random moments with the mouse anywhere, so each
frame is first run whole to find the mouse, and the crops are then placed around
it. Centring on the frame instead would measure how often a mouse happens to be in
the middle, which is not the question.

**The full-frame result is the reference.** "Accuracy" here means agreement with
what the same model says when it can see everything, not agreement with a human
labeller. That is the right comparison for deciding a crop size: the question is
what cropping costs, not what the model costs.

    python scripts/benchmark_pose.py --frames benchmark_frames

Run scripts/extract_benchmark_frames.py first to get the frames.
"""

from __future__ import annotations

import argparse
import json
import statistics
import sys
import time
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "python"))

DEFAULT_MODEL = (r"Y:\srogers\Behaviour\DEEPLABCUT_models"
                 r"\250822_wildtype_chemo_model_superanimal\project_folders"
                 r"\no_implant_superanimal-StefanRC-2025-08-22")

# Full frame down to tight. All multiples of 32, which is what the net pads to.
DEFAULT_SIZES = [1024, 768, 640, 512, 448, 384, 320, 256]

LIKELIHOOD_FLOOR = 0.6


def load_frames(folder: Path, limit: int) -> list[np.ndarray]:
    try:
        import imageio.v3 as iio
    except ImportError:
        print("imageio is needed to read the frames")
        return []
    frames = []
    for path in sorted(folder.glob("*.png"))[:limit]:
        image = iio.imread(path)
        if image.ndim == 3:
            image = image[..., 0]
        frames.append(np.ascontiguousarray(image))
    return frames


def crop_around(frame: np.ndarray, centre: tuple[float, float], size: int):
    """A square crop of ``size`` around a point, clamped inside the frame."""
    height, width = frame.shape[:2]
    size = min(size, width, height)
    x = int(round(centre[0] - size / 2))
    y = int(round(centre[1] - size / 2))
    x = max(0, min(x, width - size))
    y = max(0, min(y, height - size))
    return frame[y:y + size, x:x + size], x, y


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--model", default=DEFAULT_MODEL)
    parser.add_argument("--snapshot", default=None)
    parser.add_argument("--frames", default="benchmark_frames")
    parser.add_argument("--limit", type=int, default=24)
    parser.add_argument("--repeats", type=int, default=3)
    parser.add_argument("--sizes", default=",".join(str(s) for s in DEFAULT_SIZES))
    parser.add_argument("--eager", action="store_true",
                        help="also measure without CUDA graphs, for comparison")
    parser.add_argument("--out", default="benchmark_frames/pose_benchmark.json")
    args = parser.parse_args()

    frames = load_frames(Path(args.frames), args.limit)
    if not frames:
        print(f"No frames in {args.frames}. Run scripts/extract_benchmark_frames.py.")
        return 2
    height, width = frames[0].shape[:2]
    print(f"{len(frames)} frames at {width}x{height}")

    from head_angle import REQUIRED_PARTS, head_angle
    from pose_engine import PoseEngine

    sizes = sorted({int(s) for s in args.sizes.split(",") if s.strip()}, reverse=True)

    print("\nLoading model ...")
    started = time.perf_counter()
    engine = PoseEngine(args.model, snapshot=args.snapshot, half=True, crop_size=None)
    load_s = time.perf_counter() - started
    info = engine.describe()
    print(f"  {info['net_type']}, {info['method']}, {len(info['bodyparts'])} keypoints, "
          f"snapshot {info['snapshot']}")
    print(f"  {info['precision']}, cuda_graph={info['cuda_graph']}")
    print(f"  loaded in {load_s:.1f} s "
          f"({'network drive' if str(info['model_dir'])[:2].upper() == 'Y:' else 'local'})")

    # Reference pass: the mouse's position and keypoints with the whole frame
    # visible. Everything below is measured against this.
    print("\nFinding the mouse in each frame (full frame) ...")
    engine.warmup(frames[0], rounds=5, crop_size=None)
    reference = []
    for frame in frames:
        keypoints = engine.infer(frame, crop_size=None)
        result = head_angle(keypoints)
        if result is None:
            reference.append(None)
            continue
        needed = [keypoints[p] for p in REQUIRED_PARTS if p in keypoints]
        if min(k.likelihood for k in needed) < LIKELIHOOD_FLOOR:
            reference.append(None)        # no confident mouse; not a fair test case
            continue
        reference.append((result.ear_midpoint, keypoints, result.bearing))

    usable = [i for i, r in enumerate(reference) if r is not None]
    print(f"  {len(usable)} of {len(frames)} frames have a confident mouse")
    if not usable:
        print("No frames with a confident detection. Check the model and frames.")
        return 1

    full_times = [engine.last_inference_ms]
    for _ in range(args.repeats):
        for frame in frames:
            engine.infer(frame, crop_size=None)
            full_times.append(engine.last_inference_ms)
    full_median = statistics.median(full_times)
    print(f"  full frame {width}x{height}: {full_median:.1f} ms median")

    results = [{
        "crop": max(width, height), "median_ms": full_median,
        "found_pct": 100.0, "max_bearing_error": 0.0, "max_coord_error": 0.0,
        "frames": len(usable), "note": "full frame, the reference",
    }]

    print()
    print(f"  {'crop':>6} {'median':>8} {'p90':>8} {'cold':>7}   "
          f"{'found':>6}  {'bearing err':>12}  {'coord err':>10}")
    print(f"  {'':->6} {'':->8} {'':->8} {'':->7}   {'':->6}  {'':->12}  {'':->10}")

    for size in sizes:
        if size > min(width, height):
            continue

        sample, _, _ = crop_around(frames[usable[0]],
                                   reference[usable[0]][0], size)

        cold_started = time.perf_counter()
        engine.infer_array(sample)
        cold_ms = (time.perf_counter() - cold_started) * 1000.0

        for _ in range(5):
            engine.infer_array(sample)

        timings: list[float] = []
        found = 0
        bearing_errors: list[float] = []
        coord_errors: list[float] = []

        for index in usable:
            frame = frames[index]
            centre, ref_keypoints, ref_bearing = reference[index]
            view, offset_x, offset_y = crop_around(frame, centre, size)

            for _ in range(args.repeats):
                started = time.perf_counter()
                coordinates, scores = engine.infer_array(view)
                import torch
                torch.cuda.synchronize()
                timings.append((time.perf_counter() - started) * 1000.0)

            from head_angle import Keypoint
            keypoints = {
                name: Keypoint(float(coordinates[i][0]) + offset_x,
                               float(coordinates[i][1]) + offset_y,
                               float(scores[i]))
                for i, name in enumerate(engine.bodyparts)
            }

            needed = [keypoints[p] for p in REQUIRED_PARTS if p in keypoints]
            if needed and min(k.likelihood for k in needed) >= LIKELIHOOD_FLOOR:
                found += 1
                for part in REQUIRED_PARTS:
                    coord_errors.append(
                        max(abs(keypoints[part].x - ref_keypoints[part].x),
                            abs(keypoints[part].y - ref_keypoints[part].y)))
                result = head_angle(keypoints)
                if result is not None:
                    difference = abs((result.bearing - ref_bearing + 180) % 360 - 180)
                    bearing_errors.append(difference)

        timings.sort()
        median = statistics.median(timings)
        p90 = timings[int(len(timings) * 0.9)]
        found_pct = 100.0 * found / len(usable)
        max_bearing = max(bearing_errors) if bearing_errors else float("nan")
        max_coord = max(coord_errors) if coord_errors else float("nan")

        print(f"  {size:>6} {median:>7.1f}m {p90:>7.1f}m {cold_ms:>6.0f}m   "
              f"{found_pct:>5.0f}%  {max_bearing:>11.2f}d  {max_coord:>9.1f}p")

        results.append({
            "crop": size, "median_ms": median, "p90_ms": p90, "cold_ms": cold_ms,
            "found_pct": found_pct,
            "max_bearing_error": None if np.isnan(max_bearing) else max_bearing,
            "max_coord_error": None if np.isnan(max_coord) else max_coord,
            "frames": len(usable),
        })

    Path(args.out).parent.mkdir(parents=True, exist_ok=True)
    Path(args.out).write_text(json.dumps(results, indent=2), encoding="utf-8")
    print(f"\nWritten to {args.out}")

    # The recommendation, stated against the plan's targets.
    print()
    clean = [r for r in results
             if r["found_pct"] >= 99 and (r["max_bearing_error"] or 0) < 1.0]
    if not clean:
        print("No crop size reproduced the full-frame answer reliably. Use the "
              "full frame, or look at the detections before trusting a crop.")
        return 1

    best = min(clean, key=lambda r: r["median_ms"])
    print(f"Smallest crop that still reproduces the full-frame heading: "
          f"{best['crop']} px, {best['median_ms']:.1f} ms median.")
    print(f"Against a 10 ms frame period at 100 fps and ~2 ms of everything else, "
          f"that leaves plenty inside a 30 ms round trip.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
