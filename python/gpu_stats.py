"""GPU health, cheap enough to sample continuously.

Through NVML directly rather than by running ``nvidia-smi``: a full sample costs
**0.061 ms** measured, against tens of milliseconds to spawn a subprocess. That
difference is what makes it reasonable to poll a few times a second and put the
result on screen.

Two of the fields here are not ordinary telemetry and are worth saying why:

**Clock and throttle state.** An idle GPU drops its graphics clock to a few
hundred MHz and its memory clock with it, and an inference once a second then
costs about ten times one run back to back -- 94 ms against 8 ms, measured. It is
entirely silent: nothing fails, the answer is just late. Surfacing the clocks and
the throttle reason makes the single largest latency factor in this system
visible instead of mysterious.

**Encoder sessions.** The recorder encodes HEVC on the same card the model runs
on. If the two ever contend this is the only place it would show.

``nvidia-ml-py`` provides the ``pynvml`` module. ``torch.cuda.utilization()`` and
friends are thin wrappers over the same library and raise without it, so there is
nothing to be gained by going through torch.
"""

from __future__ import annotations

import threading
from dataclasses import dataclass, field
from typing import Optional

# NVML's throttle bits, in the order worth reporting them. GpuIdle first because
# it is the one that actually bites here.
_THROTTLE_BITS = [
    (0x0000000001, "GpuIdle"),
    (0x0000000002, "AppClocksSetting"),
    (0x0000000004, "SwPowerCap"),
    (0x0000000008, "HwSlowdown"),
    (0x0000000010, "SyncBoost"),
    (0x0000000020, "SwThermalSlowdown"),
    (0x0000000040, "HwThermalSlowdown"),
    (0x0000000080, "HwPowerBrakeSlowdown"),
    (0x0000000100, "DisplayClockSetting"),
]

# A clock this far below its maximum on an otherwise quiet machine means the card
# is managing it, not that something pinned it.
_LOCKED_FRACTION = 0.6


@dataclass
class GpuSample:
    name: str = ""
    util_percent: int = 0
    memory_bus_percent: int = 0
    vram_used_gb: float = 0.0
    vram_total_gb: float = 0.0
    temperature_c: int = 0
    power_w: float = 0.0
    power_limit_w: float = 0.0
    sm_mhz: int = 0
    sm_max_mhz: int = 0
    mem_mhz: int = 0
    mem_max_mhz: int = 0
    throttle_reasons: list[str] = field(default_factory=list)
    encoder_sessions: int = 0
    encoder_fps: int = 0
    encoder_latency_us: int = 0
    available: bool = False
    error: Optional[str] = None

    @property
    def sm_locked(self) -> bool:
        return self.sm_max_mhz > 0 and self.sm_mhz > self.sm_max_mhz * _LOCKED_FRACTION

    @property
    def mem_locked(self) -> bool:
        return self.mem_max_mhz > 0 and self.mem_mhz > self.mem_max_mhz * _LOCKED_FRACTION

    @property
    def clocks_locked(self) -> bool:
        return self.sm_locked and self.mem_locked

    def to_dict(self) -> dict:
        return {
            "available": self.available,
            "error": self.error,
            "name": self.name,
            "util_percent": self.util_percent,
            "memory_bus_percent": self.memory_bus_percent,
            "vram_used_gb": round(self.vram_used_gb, 2),
            "vram_total_gb": round(self.vram_total_gb, 1),
            "temperature_c": self.temperature_c,
            "power_w": round(self.power_w, 1),
            "power_limit_w": round(self.power_limit_w),
            "sm_mhz": self.sm_mhz, "sm_max_mhz": self.sm_max_mhz,
            "mem_mhz": self.mem_mhz, "mem_max_mhz": self.mem_max_mhz,
            "sm_locked": self.sm_locked, "mem_locked": self.mem_locked,
            "clocks_locked": self.clocks_locked,
            "throttle_reasons": self.throttle_reasons,
            "encoder_sessions": self.encoder_sessions,
            "encoder_fps": self.encoder_fps,
            "encoder_latency_us": self.encoder_latency_us,
        }


class GpuMonitor:
    """Samples one GPU. Safe to call from any thread; never raises."""

    def __init__(self, index: int = 0):
        self.index = index
        self._lock = threading.Lock()
        self._nvml = None
        self._handle = None
        self._failed: Optional[str] = None
        self._open()

    def _open(self) -> None:
        try:
            import pynvml
            pynvml.nvmlInit()
            self._nvml = pynvml
            self._handle = pynvml.nvmlDeviceGetHandleByIndex(self.index)
        except Exception as exc:                              # noqa: BLE001
            # No NVML, no NVIDIA card, or a driver mismatch. The pose system works
            # perfectly well without any of this; only the health bar is lost.
            self._failed = f"{type(exc).__name__}: {exc}"
            self._nvml = None

    def sample(self) -> GpuSample:
        if self._nvml is None:
            return GpuSample(available=False, error=self._failed)

        nvml, handle = self._nvml, self._handle
        out = GpuSample(available=True)
        with self._lock:
            try:
                name = nvml.nvmlDeviceGetName(handle)
                out.name = name.decode() if isinstance(name, bytes) else str(name)

                rates = nvml.nvmlDeviceGetUtilizationRates(handle)
                out.util_percent = int(rates.gpu)
                out.memory_bus_percent = int(rates.memory)

                memory = nvml.nvmlDeviceGetMemoryInfo(handle)
                out.vram_used_gb = memory.used / 2**30
                out.vram_total_gb = memory.total / 2**30

                out.temperature_c = int(nvml.nvmlDeviceGetTemperature(
                    handle, nvml.NVML_TEMPERATURE_GPU))
                out.power_w = nvml.nvmlDeviceGetPowerUsage(handle) / 1000.0
                out.sm_mhz = int(nvml.nvmlDeviceGetClockInfo(handle, nvml.NVML_CLOCK_SM))
                out.mem_mhz = int(nvml.nvmlDeviceGetClockInfo(handle, nvml.NVML_CLOCK_MEM))
            except Exception as exc:                          # noqa: BLE001
                return GpuSample(available=False, error=f"{type(exc).__name__}: {exc}")

            # Each of these is optional: some are unsupported on some cards and a
            # missing one must not cost the whole sample.
            for read in (
                lambda: setattr(out, "power_limit_w",
                                nvml.nvmlDeviceGetEnforcedPowerLimit(handle) / 1000.0),
                lambda: setattr(out, "sm_max_mhz", int(nvml.nvmlDeviceGetMaxClockInfo(
                    handle, nvml.NVML_CLOCK_SM))),
                lambda: setattr(out, "mem_max_mhz", int(nvml.nvmlDeviceGetMaxClockInfo(
                    handle, nvml.NVML_CLOCK_MEM))),
                lambda: setattr(out, "throttle_reasons", _decode_throttle(
                    nvml.nvmlDeviceGetCurrentClocksThrottleReasons(handle))),
                lambda: _read_encoder(nvml, handle, out),
            ):
                try:
                    read()
                except Exception:                             # noqa: BLE001
                    pass
        return out


def _decode_throttle(mask: int) -> list[str]:
    if not mask:
        return []
    return [name for bit, name in _THROTTLE_BITS if mask & bit]


def _read_encoder(nvml, handle, out: GpuSample) -> None:
    sessions, fps, latency = nvml.nvmlDeviceGetEncoderStats(handle)
    out.encoder_sessions = int(sessions)
    out.encoder_fps = int(fps)
    out.encoder_latency_us = int(latency)
