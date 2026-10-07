"""Does the live heading calculation agree with the offline analysis?

This is the gate on the whole live-pose idea. If the live system and the offline
pipeline disagree about what a mouse's heading was on a given frame, then no live
decision can be checked after the fact and none of the recorded data means what it
appears to.

So this test does not assert against numbers someone typed in. It imports the real
``Session_nwb.find_angles`` from hex_behav_analysis, binds it to a stub carrying
only the handful of attributes it reads, and runs both implementations over the
same randomised keypoints -- including cases built specifically to trigger the
ear-swap flip correction and the spine fallback.

Run it with an interpreter that can import hex_behav_analysis:

    C:\\Users\\Admin\\anaconda3\\envs\\behaviour\\python.exe tests/test_head_angle.py

It skips, rather than fails, if that library is not importable -- the live code has
to stand alone, so this comparison is a check on it and not a dependency of it.
"""

from __future__ import annotations

import math
import random
import sys
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "python"))

from head_angle import Keypoint, REQUIRED_PARTS, head_angle  # noqa: E402

HEX_BEHAV = Path(r"C:\dev\projects\hex_behav\hex_behav_analysis")

# The rig 3 port table from Session_nwb._get_port_coordinates. Index 0 is
# LED_1/SENSOR1 through index 5 for LED_6/SENSOR6.
RIG3_PORTS = [(840, 100), (375, 100), (160, 520), (410, 920), (860, 880), (1110, 490)]


def load_offline():
    """The real find_angles, bound to a stub. Returns None if unavailable."""
    if str(HEX_BEHAV) not in sys.path:
        sys.path.insert(0, str(HEX_BEHAV))
    try:
        import pandas as pd
        from hex_behav_analysis.utils.Session_nwb import Session
    except Exception as exc:                                  # noqa: BLE001
        print(f"SKIP: cannot import hex_behav_analysis ({exc})")
        return None

    class Stub:
        """Only what find_angles actually touches."""
        def __init__(self, ports):
            self.port_coordinates = ports
            self.rig_id = 5

        def _get_port_coordinates(self):
            return self.port_coordinates

        find_angles = Session.find_angles

    def call(keypoints, ports, correct_port):
        columns, data = [], []
        for name, point in keypoints.items():
            for coord, value in (("x", point.x), ("y", point.y),
                                 ("likelihood", point.likelihood)):
                columns.append((name, coord))
                data.append(value)
        frame = pd.DataFrame([data], columns=pd.MultiIndex.from_tuples(columns))
        trial = {
            "DLC_data": frame,
            "correct_port": str(correct_port),
            "next_sensor": {},
        }
        return Stub(ports).find_angles(trial, buffer=1)

    return call


def random_pose(rng, *, ears_close=False, swap_ears=False):
    """A plausible mouse somewhere near the middle of the arena."""
    centre_x = rng.uniform(500, 780)
    centre_y = rng.uniform(380, 640)
    facing = rng.uniform(0, 2 * math.pi)

    # Ear separation: normally comfortably above the 50 px threshold, or
    # deliberately below it to force the spine fallback.
    half = rng.uniform(8, 20) if ears_close else rng.uniform(30, 55)

    # Ears sit either side of the facing direction, so the perpendicular of the
    # ear vector is the heading.
    across = facing + math.pi / 2
    left = (centre_x - half * math.cos(across), centre_y + half * math.sin(across))
    right = (centre_x + half * math.cos(across), centre_y - half * math.sin(across))
    if swap_ears:
        left, right = right, left

    keypoints = {
        "left_ear": Keypoint(left[0], left[1], rng.uniform(0.7, 1.0)),
        "right_ear": Keypoint(right[0], right[1], rng.uniform(0.7, 1.0)),
    }

    # Spine running backwards from the head, with a little noise so the fitted
    # line is not perfectly straight.
    for i, name in enumerate(("spine_1", "spine_2", "spine_3", "spine_4"), start=1):
        distance = 18.0 * i
        keypoints[name] = Keypoint(
            centre_x - distance * math.cos(facing) + rng.uniform(-3, 3),
            centre_y + distance * math.sin(facing) + rng.uniform(-3, 3),
            rng.uniform(0.6, 1.0),
        )
    return keypoints


def compare(offline, keypoints, correct_port, label, failures):
    live = head_angle(keypoints, port_coordinates=RIG3_PORTS, correct_port=correct_port)
    ref = offline(keypoints, RIG3_PORTS, correct_port)

    if ref is None:
        if live is not None:
            failures.append(f"{label}: offline refused, live returned a bearing")
        return
    if live is None:
        failures.append(f"{label}: live refused, offline returned {ref['bearing']:.3f}")
        return

    checks = [
        ("bearing", live.bearing, ref["bearing"]),
        ("midpoint_x", live.midpoint[0], ref["midpoint"][0]),
        ("midpoint_y", live.midpoint[1], ref["midpoint"][1]),
        ("ear_distance", live.ear_distance, ref["ear_distance"]),
        ("cue_angle", live.cue_presentation_angle, ref["cue_presentation_angle"]),
    ]
    for name, got, want in checks:
        if got is None or want is None:
            if got is not want:
                failures.append(f"{label}: {name} live={got} offline={want}")
            continue
        if not math.isclose(got, want, rel_tol=1e-9, abs_tol=1e-7):
            failures.append(
                f"{label}: {name} live={got:.6f} offline={want:.6f} "
                f"(diff {abs(got - want):.2e})")

    if live.angle_correction_method != ref["angle_correction_method"]:
        failures.append(
            f"{label}: method live={live.angle_correction_method} "
            f"offline={ref['angle_correction_method']}")
    if live.spine_data_available != ref["spine_data_available"]:
        failures.append(f"{label}: spine_data_available disagrees")


def main() -> int:
    offline = load_offline()
    if offline is None:
        return 0

    rng = random.Random(20261007)
    failures: list[str] = []
    methods: dict[str, int] = {}

    cases = (
        ("normal", dict()),
        ("swapped ears", dict(swap_ears=True)),
        ("ears close", dict(ears_close=True)),
        ("ears close + swapped", dict(ears_close=True, swap_ears=True)),
    )

    for label, kwargs in cases:
        for i in range(150):
            keypoints = random_pose(rng, **kwargs)
            port = rng.randint(1, 6)
            result = head_angle(keypoints, port_coordinates=RIG3_PORTS, correct_port=port)
            if result is not None:
                methods[result.angle_correction_method] = \
                    methods.get(result.angle_correction_method, 0) + 1
            compare(offline, keypoints, port, f"{label} #{i}", failures)

    # A missing spine point must make the whole spine unavailable, as offline.
    for i in range(50):
        keypoints = random_pose(rng)
        del keypoints[rng.choice(["spine_1", "spine_2", "spine_3", "spine_4"])]
        compare(offline, keypoints, 1, f"spine missing #{i}", failures)

    # Non-finite coordinates.
    for i in range(20):
        keypoints = random_pose(rng)
        keypoints["spine_2"] = Keypoint(float("nan"), float("nan"), 0.1)
        compare(offline, keypoints, 1, f"spine nan #{i}", failures)

    # Agreement with the offline code proves the port is faithful. It does not
    # prove the flip correction is useful -- both could be wrong together. The
    # check that it does its job is this: swapping the two ear labels is exactly
    # the mistake DLC makes, so doing it deliberately must not change the answer.
    swap_failures = 0
    swap_tested = 0
    for _ in range(300):
        keypoints = random_pose(rng)
        swapped = dict(keypoints)
        swapped["left_ear"], swapped["right_ear"] =             keypoints["right_ear"], keypoints["left_ear"]

        straight = head_angle(keypoints)
        crossed = head_angle(swapped)
        if straight is None or crossed is None:
            continue
        swap_tested += 1
        difference = abs((straight.bearing - crossed.bearing + 180) % 360 - 180)
        if difference > 1e-6:
            swap_failures += 1
            if swap_failures <= 5:
                failures.append(
                    f"ear swap changed the bearing by {difference:.3f} deg "
                    f"({straight.bearing:.3f} -> {crossed.bearing:.3f}, "
                    f"methods {straight.angle_correction_method} / "
                    f"{crossed.angle_correction_method})")

    print(f"ear-swap invariance      {swap_tested - swap_failures}/{swap_tested} "
          f"recovered the same bearing")
    print()

    print("methods exercised:")
    for name, count in sorted(methods.items()):
        print(f"  {name:28s} {count}")
    print()

    missing = [m for m in ("none", "ear_flip_corrected", "spine_based")
               if m not in methods]
    if missing:
        print(f"WARNING: these paths were never exercised: {', '.join(missing)}")

    if failures:
        print(f"{len(failures)} disagreement(s) with the offline analysis:")
        for failure in failures[:25]:
            print(f"  {failure}")
        if len(failures) > 25:
            print(f"  ... and {len(failures) - 25} more")
        return 1

    print("Live and offline agree on every case.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
