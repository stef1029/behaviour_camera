"""Running a DeepLabCut 3 pose model on single frames, fast enough to decide on.

Loads a DLC 3 PyTorch model and runs it on one frame at a time, which is the
opposite of what the normal DLC pipeline is built for -- that batches a whole
video through a DataLoader and cares about throughput. Here only latency matters:
a protocol asks where the mouse is looking and has to get an answer before the
moment it is asking about has passed.

Two decisions worth knowing about.

**Not dlclive.** DeepLabCut-Live's latest release is 1.1.0, from the TensorFlow
era, and does not load DLC 3 PyTorch snapshots. DeepLabCut 3's own inference API
does, is maintained, and is what DLC itself runs, so that is what this uses.

**CUDA graphs, and why they turned out to matter more than cropping.** HRNet-w32
is built from four parallel resolution branches that fuse repeatedly, which comes
to 1834 parameter tensors and well over a thousand CUDA kernel launches for one
frame. Measured on an RTX 4000 Ada, eager inference takes about 33 ms *whatever
the input size* -- 448x448 and 1280x1280 cost the same, because almost all of that
is launch overhead with the GPU sitting idle between kernels. Cropping a
launch-bound model saves nothing at all.

Capturing the forward pass as a CUDA graph and replaying it removes that overhead
and the time becomes compute-bound again, at which point cropping helps as
expected:

    crop    eager    graphed
     448   32.9 ms    6.0 ms    5.5x
     640   32.6 ms    7.8 ms    4.2x
    1024   32.7 ms   15.7 ms    2.1x

A graph is tied to one input shape, so changing crop size re-captures. Capture is
guarded and falls back to eager, because a wrong answer quickly is worse than a
right answer slowly.

**Centre crop, not downscale.** The mouse is on the scales when a reading is
taken, so it is reliably near the middle of the frame and most of the frame can be
thrown away. That is worth roughly an order of magnitude of compute. It is also
better for accuracy than downscaling, which is the less obvious half: this model
was trained on 448x448 crops at close to native scale, so a centre crop keeps the
mouse at the size the model expects, while shrinking the whole frame would make it
smaller than anything in training.

Coordinates coming out are always in **full-frame pixel space**, mapped back from
the crop, because the port coordinates and the two pixel thresholds in
``head_angle`` all live in that space.
"""

from __future__ import annotations

import glob
import os
import re
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Optional

import numpy as np

# ImageNet statistics, which is what DLC's normalize_images uses.
IMAGENET_MEAN = np.array([0.485, 0.456, 0.406], dtype=np.float32)
IMAGENET_STD = np.array([0.229, 0.224, 0.225], dtype=np.float32)


def find_pose_config(model_dir: str | Path) -> Path:
    """The pytorch_config.yaml inside a DLC project folder.

    Takes either the project folder or the train folder itself, because both are
    reasonable things to put in a config file and guessing wrong is a confusing
    error much later.
    """
    model_dir = Path(model_dir)
    direct = model_dir / "pytorch_config.yaml"
    if direct.exists():
        return direct

    matches = sorted(model_dir.glob("dlc-models-pytorch/*/*/train/pytorch_config.yaml"))
    if not matches:
        matches = sorted(model_dir.glob("**/train/pytorch_config.yaml"))
    if not matches:
        raise FileNotFoundError(
            f"no pytorch_config.yaml under {model_dir}. Point at a DeepLabCut 3 "
            f"project folder, or at the train folder inside one.")
    if len(matches) > 1:
        raise FileNotFoundError(
            f"{len(matches)} shuffles under {model_dir}; name the train folder "
            f"explicitly so it is recorded which one ran:\n  " +
            "\n  ".join(str(m.parent) for m in matches))
    return matches[0]


def choose_snapshot(train_dir: str | Path, snapshot: Optional[str] = None) -> Path:
    """Pick which weights to load.

    ``snapshot-best-*.pt`` wins when DeepLabCut has marked one, because that is
    the epoch that scored best on the held-out set rather than merely the last.
    Otherwise the highest-numbered snapshot. The detector snapshot is skipped: it
    is an artefact of an aborted run and this model is bottom-up, with no
    detector in the loop.
    """
    train_dir = Path(train_dir)
    if snapshot:
        path = train_dir / snapshot
        if not path.exists():
            raise FileNotFoundError(f"no snapshot {snapshot} in {train_dir}")
        return path

    best = sorted(train_dir.glob("snapshot-best-*.pt"))
    if best:
        return best[-1]

    numbered = []
    for path in train_dir.glob("snapshot-*.pt"):
        if "detector" in path.name:
            continue
        match = re.search(r"snapshot-(\d+)\.pt$", path.name)
        if match:
            numbered.append((int(match.group(1)), path))
    if not numbered:
        raise FileNotFoundError(f"no snapshots in {train_dir}")
    return max(numbered)[1]


@dataclass
class Crop:
    x: int
    y: int
    width: int
    height: int

    @property
    def is_whole_frame(self) -> bool:
        return self.x == 0 and self.y == 0


def centre_crop(frame_width: int, frame_height: int, size: int,
                centre: Optional[tuple[int, int]] = None) -> Crop:
    """A square crop of ``size``, clamped to stay inside the frame.

    ``centre`` defaults to the middle of the frame. On the rigs the scales sit
    near there, and a reading is only taken while the mouse is on them.
    """
    size = min(size, frame_width, frame_height)
    cx, cy = centre if centre is not None else (frame_width // 2, frame_height // 2)
    x = int(round(cx - size / 2))
    y = int(round(cy - size / 2))
    x = max(0, min(x, frame_width - size))
    y = max(0, min(y, frame_height - size))
    return Crop(x, y, size, size)


class PoseEngine:
    """A loaded model, ready to answer one frame at a time."""

    def __init__(
        self,
        model_dir: str | Path,
        *,
        snapshot: Optional[str] = None,
        device: str = "cuda",
        half: bool = True,
        crop_size: Optional[int] = 448,
        crop_centre: Optional[tuple[int, int]] = None,
        use_cuda_graph: bool = True,
    ):
        import torch
        import yaml
        from deeplabcut.pose_estimation_pytorch.models import PoseModel

        self._torch = torch
        self.crop_size = crop_size
        self.crop_centre = crop_centre

        self.config_path = find_pose_config(model_dir)
        self.train_dir = self.config_path.parent
        self.snapshot_path = choose_snapshot(self.train_dir, snapshot)

        with open(self.config_path, "r", encoding="utf-8") as handle:
            cfg = yaml.safe_load(handle)

        self.bodyparts: list[str] = list(cfg["metadata"]["bodyparts"])
        self.net_type: str = cfg.get("net_type", "unknown")
        self.method: str = cfg.get("method", "bu")

        if device == "cuda" and not torch.cuda.is_available():
            raise RuntimeError(
                "CUDA is not available, and running this model on the CPU is far "
                "too slow to decide on. Check the driver and the torch build.")
        self.device = torch.device(device)

        # half precision roughly halves the time on Ada tensor cores and costs
        # nothing measurable in keypoint accuracy at this resolution. Checked by
        # the benchmark rather than assumed.
        self.dtype = torch.float16 if (half and device == "cuda") else torch.float32

        model = PoseModel.build(cfg["model"], pretrained_backbone=False)
        state = torch.load(self.snapshot_path, map_location="cpu", weights_only=False)
        model.load_state_dict(state["model"])
        model.eval().to(self.device)
        if self.dtype == torch.float16:
            model.half()
        # channels_last suits the tensor cores and is worth a few ms on its own.
        if self.device.type == "cuda":
            model = model.to(memory_format=torch.channels_last)
        self.model = model

        self.use_cuda_graph = use_cuda_graph and self.device.type == "cuda"
        self._graphs: dict[tuple[int, int], tuple] = {}
        self.graph_failed_reason: Optional[str] = None

        self._mean = torch.tensor(IMAGENET_MEAN, device=self.device,
                                  dtype=self.dtype).view(1, 3, 1, 1)
        self._std = torch.tensor(IMAGENET_STD, device=self.device,
                                 dtype=self.dtype).view(1, 3, 1, 1)

        self.last_inference_ms = 0.0

    # ----- the hot path -----

    def _to_tensor(self, crop_image: np.ndarray):
        """Mono8 crop to a normalised NCHW tensor on the GPU.

        The model wants RGB, so the single channel is repeated. Doing that on the
        GPU rather than in numpy saves copying three times as many bytes across
        the bus, which is not nothing at 100 fps.
        """
        torch = self._torch
        tensor = torch.from_numpy(np.ascontiguousarray(crop_image))
        tensor = tensor.to(self.device, non_blocking=True)
        tensor = tensor.to(self.dtype).div_(255.0)
        tensor = tensor.unsqueeze(0).unsqueeze(0).expand(1, 3, -1, -1)
        normalised = (tensor - self._mean) / self._std
        if self.device.type == "cuda":
            normalised = normalised.contiguous(memory_format=self._torch.channels_last)
        return normalised

    def _capture_graph(self, shape: tuple[int, int]):
        """Record the forward pass for one input shape so it can be replayed.

        Capture needs a few warmup iterations on a side stream first, or cuDNN
        picks its algorithms during capture and bakes in allocations that do not
        survive replay. Returns None if capture fails for any reason, and the
        caller falls back to eager.
        """
        torch = self._torch
        height, width = shape
        try:
            static_in = torch.zeros(1, 3, height, width, device=self.device,
                                    dtype=self.dtype)
            static_in = static_in.contiguous(memory_format=torch.channels_last)

            side = torch.cuda.Stream()
            side.wait_stream(torch.cuda.current_stream())
            with torch.cuda.stream(side):
                with torch.inference_mode():
                    for _ in range(3):
                        self.model(static_in)
            torch.cuda.current_stream().wait_stream(side)

            graph = torch.cuda.CUDAGraph()
            with torch.inference_mode():
                with torch.cuda.graph(graph):
                    static_out = self.model(static_in)
            torch.cuda.synchronize()
            return graph, static_in, static_out
        except Exception as exc:                              # noqa: BLE001
            # A wrong answer quickly is worse than a right answer slowly, so a
            # capture that will not work cleanly is simply not used.
            self.graph_failed_reason = f"{type(exc).__name__}: {exc}"
            self.use_cuda_graph = False
            return None

    def infer_array(self, crop_image: np.ndarray) -> tuple[np.ndarray, np.ndarray]:
        """Raw model output for an already-cropped image.

        Returns (coordinates in crop space, scores), both per bodypart.
        """
        torch = self._torch
        shape = (crop_image.shape[0], crop_image.shape[1])

        if self.use_cuda_graph:
            entry = self._graphs.get(shape)
            if entry is None:
                entry = self._capture_graph(shape)
                if entry is not None:
                    self._graphs[shape] = entry
            if entry is not None:
                graph, static_in, static_out = entry
                with torch.inference_mode():
                    static_in.copy_(self._to_tensor(crop_image))
                    graph.replay()
                    # Replay writes into the captured output tensors in place, so
                    # decoding them after it returns reads this frame's result.
                    predictions = self.model.get_predictions(static_out)
                return self._unpack(predictions)

        with torch.inference_mode():
            batch = self._to_tensor(crop_image)
            outputs = self.model(batch)
            predictions = self.model.get_predictions(outputs)

        return self._unpack(predictions)

    def _unpack(self, predictions) -> tuple[np.ndarray, np.ndarray]:
        poses = predictions["bodypart"]["poses"].detach().float().cpu().numpy()
        # (batch, individual, bodypart, 3+) -> the single animal's keypoints
        pose = poses.reshape(-1, len(self.bodyparts), poses.shape[-1])[0]
        return pose[:, :2], pose[:, 2]

    def infer(self, frame: np.ndarray,
              crop_size: Optional[int] = None) -> dict[str, "object"]:
        """Keypoints for a full frame, in full-frame pixel coordinates.

        Returns a dict of bodypart name to ``head_angle.Keypoint``, which is what
        the heading calculation takes.
        """
        from head_angle import Keypoint

        # None means "whatever this engine was configured with"; 0 means "the
        # whole frame, explicitly". Conflating the two is a good way to warm up
        # the wrong input shape and then wonder where 300 ms went.
        size = crop_size if crop_size is not None else self.crop_size
        height, width = frame.shape[:2]

        if not size or size >= max(width, height):
            crop = Crop(0, 0, width, height)
            view = frame
        else:
            crop = centre_crop(width, height, size, self.crop_centre)
            view = frame[crop.y:crop.y + crop.height, crop.x:crop.x + crop.width]

        started = time.perf_counter()
        coordinates, scores = self.infer_array(view)
        if self.device.type == "cuda":
            self._torch.cuda.synchronize()
        self.last_inference_ms = (time.perf_counter() - started) * 1000.0

        self.last_crop = crop
        return {
            name: Keypoint(
                x=float(coordinates[i][0]) + crop.x,   # back to full-frame space
                y=float(coordinates[i][1]) + crop.y,
                likelihood=float(scores[i]),
            )
            for i, name in enumerate(self.bodyparts)
        }

    def warmup(self, frame: Optional[np.ndarray] = None, *, rounds: int = 8,
               crop_size: Optional[int] = None) -> list[float]:
        """Run a few throwaway inferences so the first real one is not the slowest.

        CUDA picks convolution algorithms on the first call at each input shape and
        the GPU clocks up under load, so a cold first inference can take several
        times the steady-state figure. That would land on the first trial of a
        session, which is exactly where it must not.
        """
        size = crop_size if crop_size is not None else self.crop_size
        if frame is None:
            if not size:
                raise ValueError(
                    "warmup needs either a frame or a crop size: with neither "
                    "there is no way to know which input shape to warm, and "
                    "warming the wrong one is worse than not warming at all")
            frame = np.zeros((size, size), dtype=np.uint8)

        timings = []
        for _ in range(rounds):
            self.infer(frame, crop_size=crop_size)
            timings.append(self.last_inference_ms)
        return timings

    # ----- the GPU's idea of how fast it feels like going -----

    @staticmethod
    def clock_state() -> Optional[dict]:
        """Current and maximum GPU clocks, or None if nvidia-smi will not say.

        This matters more than it sounds. An idle GPU drops its graphics clock to
        a few hundred MHz and its memory clock with it, and is slow to come back
        up - so an inference that happens once a second costs several times what
        the same inference costs back to back. Measured on an RTX 4000 Ada: 8 ms
        continuous against 94 ms with a one second gap, entirely from clocks.

        Locking both clocks makes it flat at any request rate. Nothing in this
        process can do that - it needs administrator - so the most this can do is
        notice and say so.
        """
        import subprocess
        try:
            out = subprocess.run(
                ["nvidia-smi",
                 "--query-gpu=clocks.sm,clocks.max.sm,clocks.mem,clocks.max.mem",
                 "--format=csv,noheader,nounits"],
                capture_output=True, text=True, timeout=5)
            if out.returncode != 0:
                return None
            sm, sm_max, mem, mem_max = [
                int(v.strip()) for v in out.stdout.strip().splitlines()[0].split(",")]
        except Exception:                                     # noqa: BLE001
            return None
        return {
            "sm_mhz": sm, "sm_max_mhz": sm_max,
            "mem_mhz": mem, "mem_max_mhz": mem_max,
            # Idle clocks sit at a small fraction of maximum, so anything near
            # it on a quiet machine means they are pinned.
            "locked": sm > sm_max * 0.6 and mem > mem_max * 0.6,
        }

    # ----- description, for the session record -----

    def describe(self) -> dict:
        return {
            "model_dir": str(self.train_dir),
            "snapshot": self.snapshot_path.name,
            "net_type": self.net_type,
            "method": self.method,
            "bodyparts": self.bodyparts,
            "device": str(self.device),
            "precision": "fp16" if self.dtype == self._torch.float16 else "fp32",
            "crop_size": self.crop_size,
            "crop_centre": self.crop_centre,
            "cuda_graph": self.use_cuda_graph,
            "cuda_graph_failed": self.graph_failed_reason,
        }
