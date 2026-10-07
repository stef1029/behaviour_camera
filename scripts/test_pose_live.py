"""End to end: camera, shared memory, model, snapshot. The number that matters.

Starts nothing itself -- the recorder and the pose server must already be
running. Fires a burst of snapshots the way a protocol would and reports the
round trip the protocol would actually experience, which is the figure the whole
plan is written against.

    python scripts/test_pose_live.py --port 5803 --requests 50

Exit codes: 0 within the 30 ms target, 1 within 100 ms but over target,
2 over 100 ms or not working.
"""

from __future__ import annotations

import argparse
import statistics
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "python"))

from pose_client import PoseClient  # noqa: E402

TARGET_MS = 30.0
CEILING_MS = 100.0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=5801)
    parser.add_argument("--requests", type=int, default=50)
    parser.add_argument("--interval", type=float, default=0.1,
                        help="seconds between snapshots, imitating trial spacing")
    parser.add_argument("--max-age-ms", type=float, default=100.0)
    args = parser.parse_args()

    client = PoseClient(args.host, args.port)

    status = client.status()
    if not status.get("ok"):
        print(f"Pose server not answering on {args.host}:{args.port}: "
              f"{status.get('reason')}")
        return 2

    model = status.get("model", {})
    gpu = status.get("gpu") or {}
    print(f"server up {status['uptime_s']:.0f} s, "
          f"{status['requests']} requests so far")
    print(f"  {model.get('net_type')} {model.get('precision')}, "
          f"cuda_graph={model.get('cuda_graph')}, crop={status.get('crop_size')}")
    print(f"  frame out attached: {status.get('frame_out_attached')}   "
          f"ports configured: {status.get('ports_configured')}")
    if gpu.get("available"):
        print(f"  GPU {gpu['sm_mhz']} MHz / {gpu['mem_mhz']} MHz, "
              f"clocks_locked={gpu['clocks_locked']}, "
              f"throttle={gpu['throttle_reasons'] or 'none'}")
        if not gpu["clocks_locked"]:
            print("  WARNING: clocks idling - expect several times the latency")
    print()

    round_trips: list[float] = []
    inference: list[float] = []
    ages: list[float] = []
    ok_count = 0
    reasons: dict[str, int] = {}
    methods: dict[str, int] = {}
    frame_ids: list[int] = []

    for index in range(args.requests):
        started = time.perf_counter()
        snap = client.snapshot(max_age_ms=args.max_age_ms, timeout_ms=500)
        measured = (time.perf_counter() - started) * 1000.0
        round_trips.append(measured)

        if snap.ok:
            ok_count += 1
            if snap.inference_ms is not None:
                inference.append(snap.inference_ms)
            if snap.frame_age_ms is not None:
                ages.append(snap.frame_age_ms)
            if snap.frame_id is not None:
                frame_ids.append(snap.frame_id)
            method = snap.angle_correction_method or "?"
            methods[method] = methods.get(method, 0) + 1
            if index < 5:
                ahead = snap.port_ahead(within=180)
                print(f"  heading {snap.heading:6.1f}  "
                      f"at ({snap.position[0]:4.0f},{snap.position[1]:4.0f})  "
                      f"{snap.distance_from_centre:3.0f} px out  "
                      f"ahead port {ahead} ({snap.angle_to(ahead):+6.1f})  "
                      f"closest {snap.closest_port()}  "
                      f"infer {snap.inference_ms:5.1f}  trip {measured:5.1f} ms")
        else:
            reasons[snap.reason or "?"] = reasons.get(snap.reason or "?", 0) + 1
            if index < 5:
                print(f"  not ok: {snap.reason}  (trip {measured:.1f} ms)")

        if args.interval:
            time.sleep(args.interval)

    client.close()

    print()
    print(f"requests            {args.requests}")
    print(f"readings returned   {ok_count} ({100.0 * ok_count / args.requests:.0f}%)")
    if frame_ids:
        repeats = len(frame_ids) - len(set(frame_ids))
        print(f"distinct frames     {len(set(frame_ids))} "
              f"({repeats} request(s) reused a frame)")
    if methods:
        print("correction methods  " +
              ", ".join(f"{k} {v}" for k, v in sorted(methods.items())))
    if reasons:
        print("failures:")
        for reason, count in sorted(reasons.items(), key=lambda kv: -kv[1]):
            print(f"  {count:4d}x {reason}")

    if not round_trips:
        return 2

    round_trips.sort()
    median = statistics.median(round_trips)
    p95 = round_trips[int(len(round_trips) * 0.95)]
    print()
    print(f"round trip ms       median {median:.1f}  p95 {p95:.1f}  "
          f"worst {round_trips[-1]:.1f}")
    if inference:
        print(f"  of which model    median {statistics.median(inference):.1f}")
    if ages:
        print(f"  frame age         median {statistics.median(ages):.1f}  "
              f"worst {max(ages):.1f}")

    print()
    if ok_count == 0:
        print("No readings came back. Check the recorder is running with "
              "--frame-out and that a mouse is in view.")
        return 2
    if p95 <= TARGET_MS:
        print(f"p95 {p95:.1f} ms is inside the {TARGET_MS:.0f} ms target.")
        return 0
    if p95 <= CEILING_MS:
        print(f"p95 {p95:.1f} ms misses the {TARGET_MS:.0f} ms target but is "
              f"inside the {CEILING_MS:.0f} ms ceiling.")
        return 1
    print(f"p95 {p95:.1f} ms is over the {CEILING_MS:.0f} ms ceiling.")
    return 2


if __name__ == "__main__":
    sys.exit(main())
