"""Reading frames out of the recorder's shared memory block.

The reader half of ``src/frame_out.h``. Mirrors that header's layout, so the two
files have to change together -- ``HEADER_VERSION`` here must match
``kFrameOutVersion`` there, and a mismatch raises rather than misreads pixels.

Attaching does not disturb the recorder in any way. There is no handshake, no
registration and no acknowledgement; the writer publishes whether anyone is
listening or not. A reader that stalls, crashes or runs slowly cannot cost the
recording a single frame, which is the whole reason the interface looks like this.

Typical use::

    with FrameOutReader("rig3") as reader:
        frame = reader.wait_for_new(timeout_s=1.0)
        if frame is not None:
            print(frame.frame_id, frame.image.shape)

``frame.image`` is a ``numpy`` view's copy -- a real array that stays valid after
the next frame arrives, because the copy is what the seqlock retry protects.
"""

from __future__ import annotations

import mmap
import struct
import time
from dataclasses import dataclass
from typing import Optional

import numpy as np

# Must match src/frame_out.h
MAGIC = 0x42434D31          # "BCM1"
HEADER_VERSION = 1
SLOT_COUNT = 2
HEADER_BYTES = 256
PIXEL_FORMAT_MONO8 = 1

# struct FrameOutHeader, 8-byte aligned, native order.
#   8 x uint32: magic version width height stride pixel_format frame_bytes slot_count
#   5 x uint64: sequence qpc_frequency start_qpc start_unix_ns frames_captured
_HEADER_FMT = "<8I5Q"
_HEADER_SIZE = struct.calcsize(_HEADER_FMT)

# struct FrameOutSlot: uint64 frame_id, uint64 capture_qpc, uint32 size, uint32 pad
_SLOT_FMT = "<2Q2I"
_SLOT_SIZE = struct.calcsize(_SLOT_FMT)
_SLOTS_OFFSET = _HEADER_SIZE


def name_for_rig(rig: str) -> str:
    """The block name the recorder uses. Must match FrameOut::nameForRig."""
    return "Local\\behaviour_camera_frame_" + rig


class FrameOutError(RuntimeError):
    pass


class NotRunning(FrameOutError):
    """The block exists but holds no valid header, so no recorder is publishing."""


@dataclass
class Frame:
    image: np.ndarray        # (height, width) uint8
    frame_id: int            # the camera's own ID -- the join key to video and DAQ
    sequence: int            # frames published since the recorder started
    capture_qpc: int
    capture_unix_ns: int     # derived from the header's QPC reference points
    frames_captured: int     # total the recorder has captured, for liveness


class FrameOutReader:
    """Attaches to a rig's frame-out block and copies frames out of it."""

    def __init__(self, rig: str, *, name: Optional[str] = None):
        self.name = name if name is not None else name_for_rig(rig)
        self._map: Optional[mmap.mmap] = None
        self._last_sequence = 0

        # The header has to be read before the full size is known, so map just the
        # header first and remap once the frame size is in hand.
        #
        # On Windows, mmap(-1, n, tagname=...) opens an existing mapping of that
        # name and creates one if there is none. So a missing recorder shows up as
        # a block full of zeroes rather than an error -- which is why the magic is
        # checked rather than trusted.
        try:
            probe = mmap.mmap(-1, HEADER_BYTES, tagname=self.name)
        except OSError as exc:
            raise FrameOutError(f"could not open {self.name}: {exc}") from exc

        try:
            fields = struct.unpack(_HEADER_FMT, probe[:_HEADER_SIZE])
        finally:
            probe.close()

        (magic, version, width, height, stride, pixel_format,
         frame_bytes, slot_count) = fields[:8]

        if magic == 0:
            raise NotRunning(
                f"{self.name} holds no published frames: the recorder is not "
                f"running, or was not started with --frame-out")
        if magic != MAGIC:
            raise FrameOutError(
                f"{self.name} is not a frame-out block (magic 0x{magic:08X})")
        if version != HEADER_VERSION:
            raise FrameOutError(
                f"{self.name} is version {version}, this reader expects "
                f"{HEADER_VERSION}: rebuild whichever of the two is older")
        if pixel_format != PIXEL_FORMAT_MONO8:
            raise FrameOutError(
                f"{self.name} carries pixel format {pixel_format}; only Mono8 "
                f"({PIXEL_FORMAT_MONO8}) is understood")
        if slot_count != SLOT_COUNT:
            raise FrameOutError(
                f"{self.name} has {slot_count} slots, this reader expects {SLOT_COUNT}")
        if frame_bytes < width * height:
            raise FrameOutError(
                f"{self.name} claims {frame_bytes} bytes a frame but {width}x{height} "
                f"needs {width * height}")

        self.width = width
        self.height = height
        self.stride = stride
        self.frame_bytes = frame_bytes

        total = HEADER_BYTES + frame_bytes * slot_count
        try:
            self._map = mmap.mmap(-1, total, tagname=self.name)
        except OSError as exc:
            raise FrameOutError(f"could not map {self.name} ({total} bytes): {exc}") from exc

        header = struct.unpack(_HEADER_FMT, self._map[:_HEADER_SIZE])
        self.qpc_frequency = header[9]
        self.start_qpc = header[10]
        self.start_unix_ns = header[11]

    # ----- lifetime -----

    def close(self) -> None:
        if self._map is not None:
            self._map.close()
            self._map = None

    def __enter__(self) -> "FrameOutReader":
        return self

    def __exit__(self, *exc_info) -> None:
        self.close()

    # ----- reading -----

    def _read_header_counters(self) -> tuple[int, int, int]:
        """(sequence, frames_captured, magic). Cheap: no pixels touched."""
        fields = struct.unpack(_HEADER_FMT, self._map[:_HEADER_SIZE])
        return fields[8], fields[12], fields[0]

    def _qpc_to_unix_ns(self, qpc: int) -> int:
        if self.qpc_frequency == 0:
            return 0
        return self.start_unix_ns + round(
            (qpc - self.start_qpc) * 1_000_000_000 / self.qpc_frequency)

    def latest(self, *, retries: int = 8) -> Optional[Frame]:
        """The newest published frame, or None if nothing has been published yet.

        The seqlock: take the sequence, read the slot it points at, take the
        sequence again. The writer has to have published twice for the slot being
        read to have been reused, so anything less than that is a clean read.

        At 100 fps the writer needs 20 ms to lap twice and this copy takes well
        under 1 ms, so the retry effectively never fires. It is here because
        "effectively never" is not "never".
        """
        if self._map is None:
            raise FrameOutError("reader is closed")

        for _ in range(retries):
            sequence, frames_captured, magic = self._read_header_counters()
            if magic == 0:
                raise NotRunning(f"{self.name}: the recorder has stopped")
            if sequence == 0:
                return None

            index = (sequence - 1) % SLOT_COUNT

            slot_at = _SLOTS_OFFSET + index * _SLOT_SIZE
            frame_id, capture_qpc, size, _pad = struct.unpack(
                _SLOT_FMT, self._map[slot_at:slot_at + _SLOT_SIZE])

            pixels_at = HEADER_BYTES + index * self.frame_bytes
            raw = self._map[pixels_at:pixels_at + self.height * self.stride]

            # Re-check before trusting any of it.
            sequence_after, _, _ = self._read_header_counters()
            if sequence_after - sequence > 1:
                continue        # the writer lapped us; the copy may be torn

            if size < self.height * self.stride:
                continue        # a partially filled slot; next one will be whole

            image = np.frombuffer(raw, dtype=np.uint8).reshape(self.height, self.stride)
            if self.stride != self.width:
                image = image[:, :self.width]

            self._last_sequence = sequence
            return Frame(
                image=image,
                frame_id=frame_id,
                sequence=sequence,
                capture_qpc=capture_qpc,
                capture_unix_ns=self._qpc_to_unix_ns(capture_qpc),
                frames_captured=frames_captured,
            )

        raise FrameOutError(
            f"{self.name}: {retries} consecutive torn reads. Either the reader is "
            f"far slower than the frame rate, or something is wrong.")

    def wait_for_new(self, timeout_s: float = 1.0,
                     poll_s: float = 0.0005) -> Optional[Frame]:
        """Block until a frame newer than the last one read appears.

        Polling rather than waiting on an event, because the writer deliberately
        has no way to signal a reader -- signalling would mean a syscall per frame
        on the capture thread, which is exactly the cost this design refuses. A
        0.5 ms poll adds at most 0.5 ms of latency against a 10 ms frame period.
        """
        deadline = time.perf_counter() + timeout_s
        while True:
            sequence, _, magic = self._read_header_counters()
            if magic == 0:
                raise NotRunning(f"{self.name}: the recorder has stopped")
            if sequence > self._last_sequence:
                frame = self.latest()
                if frame is not None:
                    return frame
            if time.perf_counter() >= deadline:
                return None
            time.sleep(poll_s)
