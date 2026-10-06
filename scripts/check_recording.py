"""Verify that a recorded session is internally consistent.

Checks the things that would otherwise only surface much later, during conversion
or analysis:

  * the binary file holds exactly as many whole frames as the metadata claims
  * the frame IDs are strictly increasing (a camera re-init resets the counter,
    which breaks every downstream assumption about frame IDs being an index)
  * how many frames the camera produced but the host never received
  * the rate actually achieved, against the rate requested

Standard library only, so it runs under any Python on any rig without a venv.

    python scripts/check_recording.py <session_directory>

Exit codes: 0 = consistent, 1 = frames were dropped but the file is sound,
2 = inconsistent or unreadable.
"""

from __future__ import annotations

import json
import sys
from datetime import datetime
from pathlib import Path

# The recorder writes Mono8, one byte per pixel. Kept explicit because every size
# calculation below depends on it, and a future 16-bit format would silently
# halve every frame count if this were assumed inline.
BYTES_PER_PIXEL = 1


def find_one(directory: Path, pattern: str, what: str) -> Path | None:
    matches = sorted(directory.glob(pattern))
    if not matches:
        print(f"  MISSING   no {what} matching {pattern}")
        return None
    if len(matches) > 1:
        print(f"  note      {len(matches)} files match {pattern}, using {matches[0].name}")
    return matches[0]


def parse_timestamp(text: str) -> datetime | None:
    try:
        return datetime.strptime(text, "%y%m%d_%H%M%S")
    except (ValueError, TypeError):
        return None


def check(directory: Path) -> int:
    print(f"Checking {directory}")
    if not directory.is_dir():
        print("  FAIL      not a directory")
        return 2

    meta_path = find_one(directory, "*_Tracker_data.json", "metadata file")
    if meta_path is None:
        return 2

    try:
        meta = json.loads(meta_path.read_text())
    except (OSError, json.JSONDecodeError) as exc:
        print(f"  FAIL      could not read {meta_path.name}: {exc}")
        return 2

    try:
        width = int(meta["image_width"])
        height = int(meta["image_height"])
        requested_fps = float(meta["frame_rate"])
    except (KeyError, TypeError, ValueError) as exc:
        print(f"  FAIL      {meta_path.name} is missing expected fields: {exc}")
        return 2

    frame_ids = [int(x) for x in meta.get("frame_IDs") or []]
    pixel_format = meta.get("pixel_format", "<unrecorded>")

    print(f"  metadata  {width}x{height} {pixel_format}, {requested_fps:g} fps requested")

    # The frame-ID backup is written during the session, so it survives a crash
    # that stops the JSON from being finalised. Prefer whichever has more.
    backup_path = find_one(directory, "*_frame_ids_backup.txt", "frame-ID backup")
    if backup_path is not None:
        try:
            backup_ids = [int(line) for line in backup_path.read_text().split() if line.strip()]
        except (OSError, ValueError) as exc:
            print(f"  note      could not read {backup_path.name}: {exc}")
            backup_ids = []
        if len(backup_ids) > len(frame_ids):
            print(f"  note      backup holds {len(backup_ids)} IDs vs {len(frame_ids)} in the JSON; "
                  f"using the backup")
            frame_ids = backup_ids

    if not frame_ids:
        print("  FAIL      no frame IDs recorded; the session captured nothing")
        return 2

    status = 0

    # --- binary file size against the frame count ---------------------------
    bin_path = find_one(directory, "*_binary_video.bin", "binary video")
    if bin_path is None:
        status = 2
    else:
        frame_bytes = width * height * BYTES_PER_PIXEL
        actual = bin_path.stat().st_size
        whole, remainder = divmod(actual, frame_bytes)

        print(f"  binary    {actual / 1048576:.1f} MiB, {whole} whole frames"
              f"{f' + {remainder} trailing bytes' if remainder else ''}")

        if remainder:
            print(f"  FAIL      file does not divide into whole {frame_bytes}-byte frames; "
                  f"the last write was cut short")
            status = 2
        if whole != len(frame_ids):
            print(f"  FAIL      {whole} frames in the file but {len(frame_ids)} frame IDs "
                  f"recorded. Position in the file is the index the frame IDs refer to, "
                  f"so these must match.")
            status = 2
        elif not remainder:
            print(f"  OK        frame count matches the metadata ({whole} frames)")

    # --- frame IDs ----------------------------------------------------------
    backwards = [i for i in range(len(frame_ids) - 1) if frame_ids[i + 1] <= frame_ids[i]]
    if backwards:
        print(f"  FAIL      frame IDs go backwards at {len(backwards)} place(s) "
              f"(first at index {backwards[0]}). The camera counter was reset mid-session, "
              f"so frame IDs can no longer be used as an index.")
        status = 2
    else:
        span = frame_ids[-1] - frame_ids[0] + 1
        dropped = span - len(frame_ids)
        gaps = [(i, frame_ids[i + 1] - frame_ids[i] - 1)
                for i in range(len(frame_ids) - 1)
                if frame_ids[i + 1] != frame_ids[i] + 1]

        if dropped == 0:
            print(f"  OK        no dropped frames ({len(frame_ids)} consecutive IDs)")
        else:
            sizes = sorted(size for _, size in gaps)
            print(f"  DROPS     {dropped} frames lost in {len(gaps)} gap(s), "
                  f"{100.0 * dropped / span:.2f}% of the {span} the camera produced")
            print(f"            gap sizes: smallest {sizes[0]}, median "
                  f"{sizes[len(sizes) // 2]}, largest {sizes[-1]}")
            if sizes[-1] >= 50:
                print("            a gap of 50+ frames usually means the camera was "
                      "re-initialised, not a transient stall")
            status = max(status, 1)

    # --- achieved rate ------------------------------------------------------
    start = parse_timestamp(meta.get("start_time", ""))
    end = parse_timestamp(meta.get("end_time", ""))
    if start and end:
        seconds = (end - start).total_seconds()
        if seconds > 0:
            achieved = len(frame_ids) / seconds
            print(f"  rate      {achieved:.2f} fps over {seconds:.0f} s "
                  f"({requested_fps:g} requested)")
            # These timestamps bracket the whole run, including opening the camera
            # and building the preview window, so the figure understates the capture
            # rate — badly on a short test, where setup is a large share of the total.
            # Dropped frames above are the reliable signal, so this is only worth
            # remarking on once the session is long enough for setup to be noise.
            if seconds < 60:
                print("            (includes camera startup, so treat it as a floor)")
            elif achieved < requested_fps * 0.95:
                shortfall = 100 * (1 - achieved / requested_fps)
                print(f"  note      {shortfall:.0f}% below the requested rate")
    else:
        print("  note      start/end times missing or unparseable, cannot check the rate")

    print()
    if status == 0:
        print("  Session is consistent: every frame accounted for.")
    elif status == 1:
        print("  Session is usable: the file and the metadata agree, but frames were")
        print("  dropped in transfer. Frame IDs record which ones, so timing is intact.")
    else:
        print("  Session is INCONSISTENT. See the failures above before using this data.")
    return status


def main() -> int:
    if len(sys.argv) != 2:
        print(__doc__)
        return 2
    return check(Path(sys.argv[1]))


if __name__ == "__main__":
    sys.exit(main())
