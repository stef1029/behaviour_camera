"""Run the pose warmup on its own, to see what it will do at session startup.

    python scripts/pose_warmup_check.py --save screenshots_temp/warmup.png

Loads the model, infers over the reference images, draws what it found and shows
it for a few seconds. ``--seconds 0`` skips the window and only writes the file,
which is how to look at the output on a machine with no display.

Exit codes: 0 the model works, 1 it found nothing, 2 it would not load.
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "python"))

DEFAULT_MODEL = (r"Y:\srogers\Behaviour\DEEPLABCUT_models"
                 r"\250822_wildtype_chemo_model_superanimal\project_folders"
                 r"\no_implant_superanimal-StefanRC-2025-08-22")

# Rig 3's ports, from Session_nwb._get_port_coordinates. Index 0 is LED_1/SENSOR1.
RIG3_PORTS = [(840, 100), (375, 100), (160, 520), (410, 920), (860, 880), (1110, 490)]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--model", default=DEFAULT_MODEL)
    parser.add_argument("--snapshot", default=None)
    parser.add_argument("--images", default="reference_frames")
    parser.add_argument("--crop", type=int, default=0,
                        help="centre crop size; 0 for the whole frame")
    parser.add_argument("--seconds", type=float, default=3.0)
    parser.add_argument("--save", default=None)
    parser.add_argument("--ports", action="store_true",
                        help="draw rig 3's port positions too")
    args = parser.parse_args()

    from pose_engine import PoseEngine
    from pose_warmup import run_warmup, show

    try:
        engine = PoseEngine(args.model, snapshot=args.snapshot, half=True,
                            crop_size=None)
    except Exception as exc:                                  # noqa: BLE001
        print(f"Could not load the model: {exc}")
        return 2

    info = engine.describe()
    print(f"{info['net_type']} / {info['method']}, snapshot {info['snapshot']}")
    print(f"{info['precision']}, cuda_graph={info['cuda_graph']}, "
          f"{len(info['bodyparts'])} keypoints")
    print()

    result = run_warmup(
        engine, args.images,
        crop_size=args.crop or None,
        port_coordinates=RIG3_PORTS if args.ports else None,
    )

    for entry in result.per_image:
        if "error" in entry:
            print(f"  {entry['file']:34s} ERROR {entry['error']}")
            continue
        mark = "ok " if entry["detected"] else "MISS"
        bearing = "-" if entry["bearing"] is None else f"{entry['bearing']:6.1f}d"
        print(f"  {mark} {entry['file'][:34]:34s} {entry['ms']:6.1f} ms  "
              f"{bearing}  p={entry['min_likelihood']:.2f}  {entry['method'] or ''}")

    print()
    print(result.summary())

    show(result, seconds=args.seconds, save_to=args.save)
    if args.save:
        print(f"written to {args.save}")

    return 0 if result.ok else 1


if __name__ == "__main__":
    sys.exit(main())
