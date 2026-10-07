"""The startup self-test: prove the model works before a mouse goes in.

Runs the loaded model over a folder of reference mouse images, draws what it
found, and shows the lot in a window that closes itself. It earns its keep three
times over:

- It proves the whole GPU path works *before* a session rather than during one.
  A missing model file, a driver that needs a reboot or a CUDA mismatch all show
  up here, when there is still time to do something about it.
- It takes the cold start. The first inference at a new input shape costs 320-360
  ms against 6-8 ms warm, because cuDNN is choosing algorithms and the GPU is
  clocking up. Without a warmup that lands on the first trial of the session.
- It gives a measured latency baseline for this machine on this day, to compare
  the live numbers against. A rig that has quietly got slower is otherwise very
  hard to notice.

The drawn headings are the real check. A model that has learnt the mouse's ears
the wrong way round produces perfectly plausible numbers and obviously wrong
arrows, and this is the moment to catch that.
"""

from __future__ import annotations

import statistics
import time
from dataclasses import dataclass, field
from pathlib import Path
from typing import Optional, Sequence

import numpy as np

IMAGE_SUFFIXES = (".png", ".jpg", ".jpeg", ".tif", ".tiff", ".bmp")


@dataclass
class WarmupResult:
    images_tested: int = 0
    images_detected: int = 0
    cold_ms: float = 0.0
    median_ms: float = 0.0
    worst_ms: float = 0.0
    timings: list[float] = field(default_factory=list)
    per_image: list[dict] = field(default_factory=list)
    montage: Optional[np.ndarray] = None
    failure: Optional[str] = None

    @property
    def ok(self) -> bool:
        return self.failure is None and self.images_detected > 0

    def summary(self) -> str:
        if self.failure:
            return f"pose warmup FAILED: {self.failure}"
        return (f"pose warmup: {self.images_detected}/{self.images_tested} mice found, "
                f"{self.median_ms:.1f} ms median ({self.worst_ms:.1f} worst, "
                f"{self.cold_ms:.0f} cold)")


def find_reference_images(folder: str | Path) -> list[Path]:
    folder = Path(folder)
    if not folder.exists():
        return []
    return sorted(p for p in folder.iterdir()
                  if p.suffix.lower() in IMAGE_SUFFIXES)


def run_warmup(
    engine,
    reference_dir: str | Path,
    *,
    crop_size: int = 0,
    live_crop_size: Optional[int] = None,
    port_coordinates: Optional[Sequence[tuple[float, float]]] = None,
    min_likelihood: float = 0.6,
    max_images: int = 9,
) -> WarmupResult:
    """Infer over the reference images and build the montage to show.

    ``crop_size`` is what the reference images are run at, and defaults to the
    whole frame: a reference image is a mouse wherever it happened to be, not a
    mouse on the scales, so cropping to the centre would fail most of them and
    report a working model as broken.

    ``live_crop_size`` is the crop the session will actually use. It is warmed
    separately on a blank frame, because a CUDA graph is captured per input
    shape -- warming only the full frame would leave the first real request to
    pay a 300 ms capture, which is the exact cost this whole function exists to
    avoid.
    """
    from head_angle import REQUIRED_PARTS, head_angle
    from pose_overlay import draw_pose, label_panel, montage

    result = WarmupResult()

    try:
        import imageio.v3 as iio
    except ImportError:
        result.failure = "imageio is not installed, so the reference images cannot be read"
        return result

    paths = find_reference_images(reference_dir)[:max_images]
    if not paths:
        result.failure = (f"no reference images in {reference_dir}. Put a few frames "
                          f"of different mice there so startup can prove the model works.")
        return result

    panels = []
    for index, path in enumerate(paths):
        try:
            image = iio.imread(path)
        except Exception as exc:                              # noqa: BLE001
            result.per_image.append({"file": path.name, "error": str(exc)})
            continue
        if image.ndim == 3:
            image = image[..., 0]
        image = np.ascontiguousarray(image)

        started = time.perf_counter()
        keypoints = engine.infer(image, crop_size=crop_size)
        elapsed = (time.perf_counter() - started) * 1000.0

        if index == 0:
            # The first inference carries the cold start; it would skew the median.
            result.cold_ms = elapsed
        else:
            result.timings.append(elapsed)

        result.images_tested += 1

        angle = head_angle(keypoints, port_coordinates=port_coordinates)
        needed = [keypoints[p] for p in REQUIRED_PARTS if p in keypoints]
        worst_likelihood = min((k.likelihood for k in needed), default=0.0)
        detected = angle is not None and worst_likelihood >= min_likelihood
        if detected:
            result.images_detected += 1

        result.per_image.append({
            "file": path.name,
            "ms": round(elapsed, 2),
            "detected": detected,
            "min_likelihood": round(worst_likelihood, 3),
            "bearing": None if angle is None else round(angle.bearing, 2),
            "method": None if angle is None else angle.angle_correction_method,
        })

        panel = draw_pose(image, keypoints, angle,
                          port_coordinates=port_coordinates,
                          min_likelihood=min_likelihood, scale=1.6)
        if detected:
            heading_line = (f"{angle.bearing:5.1f} deg   {angle.angle_correction_method}",
                            (0, 255, 0) if angle.angle_correction_method == "none"
                            else (0, 180, 255))
        else:
            heading_line = ("NOT FOUND", (0, 0, 255))
        panel = label_panel(panel, [
            (f"{path.name[:38]}", (220, 220, 220)),
            heading_line,
            (f"{elapsed:.1f} ms   p={worst_likelihood:.2f}", (200, 200, 200)),
        ], scale=1.6)
        panels.append(panel)

    if live_crop_size is not None and live_crop_size != crop_size:
        # Capture the graph for the shape the session will use.
        engine.warmup(rounds=5, crop_size=live_crop_size)

    if result.timings:
        result.median_ms = statistics.median(result.timings)
        result.worst_ms = max(result.timings)
    if panels:
        result.montage = montage(panels, columns=3)
    if result.images_detected == 0 and result.images_tested > 0:
        result.failure = ("the model found no mouse in any reference image. Either the "
                          "wrong model is configured, or these images are not what it "
                          "was trained on.")
    return result


def show(result: WarmupResult, *, seconds: float = 3.0,
         title: str = "Pose warmup", save_to: Optional[str | Path] = None) -> None:
    """Flash the montage up, then close it. Never blocks longer than ``seconds``.

    A startup check that waits for a keypress is a startup check that one day
    holds up a session because nobody was looking at the screen.
    """
    if result.montage is None:
        return

    if save_to is not None:
        try:
            import cv2
            Path(save_to).parent.mkdir(parents=True, exist_ok=True)
            cv2.imwrite(str(save_to), result.montage)
        except Exception:                                     # noqa: BLE001
            pass

    if seconds <= 0:
        return

    try:
        import cv2
    except ImportError:
        return

    try:
        cv2.namedWindow(title, cv2.WINDOW_NORMAL)
        height, width = result.montage.shape[:2]
        cv2.resizeWindow(title, min(width, 1400), min(height, 900))
        cv2.imshow(title, result.montage)
        deadline = time.perf_counter() + seconds
        while time.perf_counter() < deadline:
            if cv2.waitKey(50) != -1:        # any key closes it early
                break
        cv2.destroyWindow(title)
        cv2.waitKey(1)
    except Exception:                                         # noqa: BLE001
        # No display, or OpenCV built without GUI support. The warmup still did
        # its real work; only the picture is missing.
        pass
