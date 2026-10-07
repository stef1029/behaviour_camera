# DLC Live — plan of action

Live pose estimation during a session, so a protocol can decide what to do based on
where the mouse is looking. Head angle is the primary quantity; inference runs on
demand at specific moments, not continuously.

**Status: phase 1 built and measured.** Everything in the first part of this document
has been implemented and tested against the real camera and real footage. Numbers that
started as estimates are now measurements, and one of them changed the design -- see
finding 1. Phase 2, the live viewer and the snapshot API, is designed but not built.

| | |
|---|---|
| Frame out (shared memory) | built, 0.54 ms a frame, no effect on the recording |
| Heading calculation | built, proved identical to the offline analysis |
| Pose engine | built, 8-10 ms at the 640 crop |
| Pose server + client | built, **p95 11.6 ms round trip** (clocks locked) |
| Warmup self-test | built, 6/6 reference mice |
| hexcontrol peripheral | built, worked protocol gated 12/12 |
| On a real rig with a mouse | **not yet** |
| Live viewer + snapshot API | built, real window verified against real mice |

**Targets:** 30 ms from "protocol asks" to "protocol has an angle". 100 ms is the
ceiling. Cameras run at 60–120 fps, most likely 100.

---

## What was checked, and what it changed

Four findings from reading the existing code and the model files, with what measuring
them afterwards actually showed. Finding 1 in particular did not survive contact with a
benchmark, and says so.

### 1. The model is HRNet-w32, and the bottleneck was not what it looked like

`config.yaml` says `default_net_type: resnet_50`, but that is only the project
default. The shuffle that was actually trained is in
`dlc-models-pytorch/iteration-0/.../train/pytorch_config.yaml`:

```yaml
net_type: hrnet_w32
```

HRNet-w32 is more accurate than ResNet-50 and considerably more expensive, and DLC
runs it at full frame resolution (padded to a multiple of 32 — 1280×1024 already is,
exactly). The 448×448 in that config is the *training* crop, not the inference size.

This machine has an **RTX 4000 Ada (20 GB)**. The first draft of this plan estimated
40-80 ms per full-frame inference and concluded that cropping was therefore essential.
Measured, the truth was stranger and the conclusion was wrong.

Eager inference takes **about 33 ms whatever the input size**:

| crop | eager forward pass |
|---|---|
| 448x448 | 32.9 ms |
| 640x640 | 32.6 ms |
| 1280x1280 | 32.6 ms |

A model whose cost does not change when given seven times fewer pixels is not
compute-bound. HRNet-w32 is four parallel resolution branches that fuse repeatedly --
1834 parameter tensors and well over a thousand CUDA kernel launches for one frame -- so
nearly all of that 33 ms is launch overhead, with the GPU idle between kernels.
**Cropping a launch-bound model saves nothing**, which is why the original plan was
treating a symptom.

The fix is to capture the forward pass as a **CUDA graph** and replay it. That collapses
the launches into a single submission, and the model becomes compute-bound again -- at
which point cropping works exactly as expected:

| crop | eager | graphed | speedup |
|---|---|---|---|
| 448 | 32.9 ms | 6.0 ms | 5.5x |
| 640 | 32.6 ms | 7.8 ms | 4.2x |
| 1024 | 32.7 ms | 15.7 ms | 2.1x |

Graph replay is **bit-identical** to eager -- 0.000000 px difference on every keypoint of
every test frame -- so this costs no accuracy at all. Neither does fp16, which also came
out bit-identical to fp32 on this model. A graph is tied to one input shape, so changing
crop size re-captures; capture is guarded and falls back to eager, because a wrong
answer quickly is worse than a right answer slowly.

TensorRT and a smaller backbone stay in reserve, and neither is needed. Retraining for
speed is not needed either.

**Choosing the crop size.** Measured with a *fixed* frame-centre crop -- which is what a
rig actually does -- over frames selected for the mouse being near the middle, as it is
at trial start. The mouse was up to 158 px off centre, median 105 px. Accuracy is
measured against what the same model says when it can see the whole frame:

| crop | median | all keypoints found | worst bearing error |
|---|---|---|---|
| 768 | 10.1 ms | 100% | 0.01 deg |
| **640** | **8.1 ms** | **100%** | **0.32 deg** |
| 512 | 6.6 ms | 92% | - |

**640 is the default.** Below it the crop starts clipping the spine and detection falls
off a cliff, and the spine is what catches an ear swap. 768 buys more margin for 2 ms if
a rig frames its arena differently.

### 2. Port **coordinates**, not port angles

The plan previously carried `port_angles` lists from
`PP_cue_offset_heading.py`. That is the older, cruder calculation — it assumes the
mouse is at the centre of the arena.

The authoritative version in `Session_nwb.py:_get_port_coordinates` stores per-rig
**pixel coordinates** of the six ports and computes each port's angle *relative to
where the mouse actually is*:

```python
for port_x, port_y in self.port_coordinates:
    vector_x = port_x - midpoint[0]
    vector_y = port_y - midpoint[1]
    port_angle_rad = math.atan2(-vector_y, vector_x)
    relative_angle_rad = port_angle_rad - mouse_heading_rad
```

This is strictly better and it is what the live version must copy. The coordinates to
move into the rigs YAML are the `new_rig_coords` table, reproduced below.

That table carries a correctness note worth preserving verbatim, because it records a
bug that was already found and fixed once:

> These tables were originally authored 0-indexed starting from the unused East port
> ("port 0"), which is physically the SENSOR6 port, so every entry sat one slot off and
> rotated all computed angles by ~60deg (one port spacing). Verified against DLC head
> position at port touch: the mouse's true location for SENSOR_k matched index k, not
> k-1, on every port across all new rigs.

Index 0 is the LED_1/SENSOR1 port. Index 5 is LED_6/SENSOR6 (East, cue unused on
visual trials). When these go into the YAML, that convention has to be stated in a
comment or it will be got wrong a third time.

### 3. Spine keypoints are not optional

You said nose can be dropped, and that is right — nose is never used. But the spine is
load-bearing. It does two jobs:

- **The left/right flip correction.** When DLC swaps the ears, the ear-vector heading
  comes out 180° wrong. The spine is the only thing that catches it.
- **The fallback heading** when the ears are too close together to give a reliable
  perpendicular (`ear_distance < 50 px`), which happens whenever the mouse is looking
  up or down relative to the camera.

So the keypoints the live system needs are `left_ear`, `right_ear`, `spine_1`,
`spine_2`, `spine_3`, `spine_4`. The model has exactly these plus `nose` and `head`.

### 4. The pixel thresholds are resolution-dependent

Two magic numbers in the offline code are in pixels:

- `minimum_ear_distance = 50` — below this, fall back to the spine
- `eyes_offset = 40` — how far forward of the ear midpoint the "eye" reference point sits

`PP_cue_offset_heading.py` declares `frame_height = 1080, frame_width = 1280`. The
current camera is **1280×1024**. The widths match, so the horizontal scale is the same
and both numbers carry over almost unchanged — but this needs stating explicitly in the
live code, and if anyone changes the sensor resolution or crops *before* computing
coordinates, both numbers and the port coordinates all break together. The live code
must compute coordinates in **full-frame pixel space** and map the crop back, never
work in crop-local coordinates.

---

## Your two questions, answered

### Where does the model have to live? Can the load location be slow?

A network drive is completely fine.

The snapshot is a single **113 MB** `.pt` file. It is read once at startup,
deserialised, and the weights are copied into GPU VRAM, where they stay for the whole
session. Nothing touches the load path again — not per frame, not per inference.

So the cost of a network drive is a few extra seconds of startup, once. Over a 1 Gb
link 113 MB is 1–2 seconds of transfer; over SMB with realistic latency, call it 5–15
seconds. Against a 1–2 hour session that is nothing, and it happens while the rest of
the rig is still booting anyway.

Keeping the models on `Y:\srogers\Behaviour\DEEPLABCUT_models` is therefore the right
choice, and better than copying them to each rig machine: one canonical copy, and no
question later about which rig ran which weights. The config will take a full path, so
a local copy remains possible if a rig has a flaky network.

One caveat: the path must be *reachable at startup*. If the share is down the session
must fail loudly at arm time, before the mouse goes in — not silently fall back to no
tracking. That is what the warmup test (below) is for.

### Do extra coordinates cost anything speed-wise?

Essentially nothing, and more importantly **you cannot drop them anyway**.

A DLC model's output layer is fixed at training time. This one produces 8 heatmaps —
one per bodypart — and it produces all 8 whether you read them or not. Asking for only
the ears and spine does not make the network smaller; it just means you ignore two of
the arrays that come back.

The cost structure is: the HRNet backbone does nearly all the work, and the heatmap
head's output channels scale linearly with keypoint count. Going from 8 keypoints to 6
would shave a low single-digit percentage off a number dominated by the backbone —
unmeasurable next to what CUDA graphs and cropping buy.

The practical consequence: **drop nose and head in the post-processing, not the model.**
And if you do retrain later for speed, the win comes from a smaller backbone
(`hrnet_w18`, `resnet_50`), not from fewer keypoints.

---

## The heading calculation, exactly as the offline analysis does it

From `Session_nwb.py:find_angles` (line 617). This is the contract — the live version
must produce the same number as the offline pipeline for the same frame, and that
equivalence should be a test.

**Step 1 — ear vector to heading.** The heading is perpendicular to the inter-ear
vector:

```python
ear_vector_x = average_right_ear[0] - average_left_ear[0]
ear_vector_y = average_right_ear[1] - average_left_ear[1]

head_vector_x = ear_vector_y
head_vector_y = -ear_vector_x

theta_rad = math.atan2(-head_vector_y, head_vector_x)
theta_deg = math.degrees(theta_rad) % 360
```

Note for anyone cross-checking against `PP_cue_offset_heading.py`, which instead writes
`atan2(-vy, vx)` then `+ 90`: these are the same operation. Rotating the y-flipped ear
vector by 90° gives exactly `atan2(ear_vector_x, ear_vector_y)`, which is what the
perpendicular form reduces to. No discrepancy to resolve.

**Step 2 — the flip correction.** Only runs when the spine disagrees by more than 90°,
and even then only on a majority vote of the spine points, so a single bad spine
detection cannot flip a good heading:

```python
spine_vector_x = spine_positions[0][0] - spine_positions[-1][0]   # spine_1 → spine_4
spine_vector_y = spine_positions[0][1] - spine_positions[-1][1]
spine_angle_deg = math.degrees(math.atan2(-spine_vector_y, spine_vector_x)) % 360

angle_diff = abs(theta_deg - spine_angle_deg)
if angle_diff > 180:
    angle_diff = 360 - angle_diff

if angle_diff > 90:
    flip_votes = 0
    for spine_pos in spine_positions:
        to_spine_x = spine_pos[0] - average_left_ear[0]
        to_spine_y = spine_pos[1] - average_left_ear[1]
        cross_product = ear_vector_x * to_spine_y - ear_vector_y * to_spine_x
        if cross_product < 0:
            flip_votes += 1
    if flip_votes > len(spine_positions) / 2:
        theta_deg = (theta_deg + 180) % 360
        angle_correction_method = "ear_flip_corrected"
```

**Step 3 — spine-based fallback** when `ear_distance < 50`: least-squares line through
spine_1..spine_4 via `np.polyfit`, direction resolved by whether spine_1 is left or
right of spine_4, with a vertical-line special case and an `atan2` fallback on
degenerate input.

**Step 4 — the reference point.** Ear midpoint normally; `spine_1` when the heading came
from the spine. Then pushed 40 px forward along the heading, approximating the eyes:

```python
eyes_offset = 40
new_midpoint_x = midpoint_x + eyes_offset * math.cos(theta_rad)
new_midpoint_y = midpoint_y - eyes_offset * math.sin(theta_rad)
```

**Step 5 — cue angle** relative to that point, as quoted in finding 2, wrapped to
(−180, 180].

`find_angles(trial, buffer=1)` — **the default averages over a single frame**, so there
is no multi-frame smoothing to replicate and no latency hidden in it. If a live request
ever wants to average over N frames for stability, that is a deliberate addition, and it
costs N frames of latency (10 ms each at 100 fps) and must be recorded as a parameter in
the output.

The live implementation returns the same dict the offline one does — `bearing`,
`midpoint`, `cue_presentation_angle`, `angle_correction_method`, `ear_distance`,
`spine_data_available`, and the ear likelihoods — because `angle_correction_method` and
the likelihoods are how you tell afterwards whether a decision was made on good data.

---

## Architecture

Three pieces, each independently testable, with the recorder's reliability ring-fenced.

```
┌─ behaviour_camera.exe (C++) ────────┐
│  capture → ring → writer → .mkv     │   recording is untouched
│         └→ shared memory (Part A)   │   one extra memcpy, ~0.3 ms
└─────────────────────────────────────┘
                 │  read-only, seqlock
┌─ dlc_live_server.py (Part B) ───────┐
│  shm → centre crop → HRNet → points │   separate process, separate GPU work
│      → heading (offline algorithm)  │
└─────────────────────────────────────┘
                 │  request/reply
┌─ hexcontrol (Part C) ───────────────┐
│  PoseTrackingPeripheral             │   optional, coordinator-managed
│  protocol: self.pose.heading()      │
└─────────────────────────────────────┘
```

Why a separate process rather than DLC inside the recorder: the recorder must not be
able to lose frames because something in the Python/CUDA stack stalled, and a crash in
PyTorch must not take the recording with it. The seqlock means the reader cannot block
the writer even in principle — the worst case is the reader notices it read a torn
frame and tries again.

### Part A — frame out, shared memory (C++, in this repo)

A named shared memory block holding the most recent frame only. Not a queue: the
consumer always wants the newest frame, and a queue would let it fall behind and make
decisions on stale data.

Seqlock, as discussed: writer increments an odd sequence number, copies, increments to
even. Reader reads the sequence, copies, re-reads the sequence, and retries if it
changed or was odd. Lock-free, and the writer never waits.

```c
struct FrameOutHeader {
    uint32_t magic, version;
    uint32_t width, height, stride;
    uint32_t pixel_format;
    uint64_t sequence;        // odd = write in progress
    uint64_t frame_id;        // the camera's ID — the join key to video and DAQ
    int64_t  capture_qpc;     // QueryPerformanceCounter at grab
    uint64_t frames_written;
};
```

`frame_id` is what makes the whole thing auditable: every live inference records which
camera frame it ran on, so any decision can be replayed against the recorded video and
lined up against the DAQ pulse train afterwards. Enabled with `--frame-out`, default
off, so nothing changes for rigs that do not use it.

Cost to the recorder: one 1.31 MB memcpy per frame, about 0.3 ms, on the capture
thread. At 100 fps that is 3% of a 10 ms frame period. If that ever proves too much it
moves to its own thread fed from the existing ring.

### Part B — the DLC live server (Python, where to put it is open)

A standalone process per rig, following the pattern of `daq_view_subprocess.py`:
launched by hexcontrol, owns its own GPU context, talks over a simple local protocol.

Responsibilities:

1. Load the model at startup; report ready or fail loudly.
2. **Warmup with the test images** (below).
3. Attach to the shared memory block.
4. On request: grab the latest frame, centre crop, infer, compute the heading with the
   algorithm above, reply.
5. Keepalive: one throwaway inference every ~500 ms when idle, so the GPU does not
   clock down between requests. Without this the first inference after a quiet period
   can take two or three times as long as the steady-state figure — which is exactly
   when it matters, because the quiet period is the inter-trial interval.
6. Log every inference to disk.

**A fixed centre crop, not ROI tracking.** The first draft of this plan tracked the
mouse between frames and cropped around its last known position, with a full-frame
re-acquire when it lost confidence. That machinery turned out to be unnecessary: a
reading is only taken while the mouse is on the scales, so it is reliably near the
middle of the frame, and a fixed 640 centre crop finds every keypoint with the mouse up
to 158 px off centre. No tracking state, no re-acquire path, nothing to go stale, and
one less thing that can be subtly wrong after a camera is nudged.

The crop centre is configurable per rig for a camera whose scales are not in the middle
of its frame. All coordinates are mapped back to full-frame space before any angle is
computed, since the port coordinates and both pixel thresholds live there.

**Model loading, and the thing to verify first.** DeepLabCut-Live was written against
TensorFlow; this model is a DLC 3.x PyTorch model. Whether the current `dlclive`
release handles PyTorch snapshots cleanly is the **first thing to establish**, before
any of this is built, because it decides the shape of Part B:

- *If `dlclive` supports it* — use it. It handles heatmap decoding and refinement
  correctly, which is fiddly code not worth reimplementing.
- *If not* — load the snapshot through DeepLabCut's own PyTorch inference API, or
  `torch.load` the weights directly. For a single-animal top-down model this is
  tractable: the heatmap head gives one channel per keypoint, and the decode is an
  argmax plus a local refinement. More code, no dependency risk, and a clearer path to
  TensorRT later.

This is a question for a half-hour experiment, not for the plan. It should be settled
first either way.

### Part C — hexcontrol integration, following the peripheral pattern

The existing framework is exactly right for this and the plan now follows it properly.

**`PoseTrackingPeripheral(Peripheral)`** in `hexcontrol/core/peripherals/pose_tracking.py`,
registered with `PeripheralsCoordinator` alongside `OpenEphysPeripheral` and
`LaserPeripheral`, with `kind = "pose_tracking"` and `display_name = "Pose Tracking"`.
It gets the lifecycle the base class defines:

| Hook | Does |
|---|---|
| `arm()` | validate the model path is reachable, note which rigs are enabled |
| `begin_for_rig()` | launch that rig's `dlc_live_server.py`, wait for ready, run warmup, record the baseline latency |
| `on_rig_stopped()` | flush and close that rig's log |
| `stop()` | terminate every server |

Because it registers with the coordinator, it appears in the peripherals sidebar panel
with a tick box, and **not ticking it is how you run without DLC** — which is the
optional-ness you asked for, obtained for free from the existing design rather than
bolted on.

**The protocol-facing side**, modelled on how trackers and scales already reach a
protocol. `BaseProtocol.set_runtime_context` grows one keyword:

```python
self._current_protocol.set_runtime_context(
    scales=scales_client,
    trackers=self._trackers,
    rig_number=rig_number,
    clock=clock,
    reward_durations=reward_durations,
    pose=pose_client,          # new; None when the peripheral is not armed
)
```

`None` when not armed, which keeps every existing protocol working untouched and gives
protocol authors an obvious guard. A protocol then reads:

```python
if self.pose is not None:
    reading = self.pose.heading(timeout_ms=100)
    if reading.ok:
        offset = reading.angle_to_port(target_port)
        if abs(offset) < 30:
            self.link.led_on(target_port)
```

`heading()` is synchronous and blocking with a timeout, because a protocol is a
sequential script and that is the shape that is easy to write against — the same reason
`self.sleep()` and the scales client look the way they do. A timeout returns
`ok=False` rather than raising, so a protocol can decide whether a missed reading
aborts the trial or just falls through to the default; and whichever it chooses, the
timeout is recorded.

### Warmup and the test images

At `begin_for_rig`, before the mouse goes in:

1. Load the model.
2. Run inference over a folder of **reference mouse images** that you supply — several
   mice of different types, so it is visible at a glance what the model handles.
3. Show them briefly in a window with keypoints and the computed heading drawn on, then
   close it automatically.
4. Record the per-image latency and the detected keypoint likelihoods to the session log.

This earns its keep three times over: it proves the GPU path works before a session
rather than during one, it gives a **measured latency baseline for that machine on that
day** to compare against the live numbers, and the drawn-on headings are a sanity check
that nothing is 180° out before any data is collected.

A failure here aborts startup, the same way the Open Ephys pre-step does.

Where the reference images live is a config question: a folder beside the models on
`Y:` is the obvious answer, so one set serves every rig.

### Saving what it generates

Every inference, written to the session folder beside the video. One row per inference,
not per frame — inference is on demand, so the record is sparse and small.

Format: CSV for a text file anyone can open, or Parquet if volume ever justifies it. One
row:

| Field | Why |
|---|---|
| `frame_id` | **the join key** — to the recorded video and, through it, to the DAQ pulse train |
| `request_time`, `reply_time` | the actual round trip the protocol experienced |
| `capture_qpc` | when the frame was exposed, so true sensor-to-decision latency is recoverable |
| `left_ear_x/y`, `right_ear_x/y`, `spine_1..4_x/y` + likelihoods | the raw pose, so the heading can be recomputed offline |
| `bearing`, `midpoint_x/y` | what the live system concluded |
| `angle_correction_method` | `none` / `ear_flip_corrected` / `spine_based` / `ears_too_close_no_spine` |
| `ear_distance`, `spine_data_available` | the inputs to that decision |
| `cue_presentation_angle` | if a target port was named in the request |
| `roi_x/y/w/h`, `was_full_frame` | which crop it ran on |
| `inference_ms` | GPU time alone, separate from the round trip |

Plus a header sidecar recording the model path, the snapshot filename, the resolved
port coordinates, both pixel thresholds, and the warmup latencies — so a session is
reproducible without having to guess which weights were loaded.

Keeping the raw keypoints matters more than it might seem: it means a live decision can
be re-derived offline and compared against what the full post-processing pipeline says
about the same frame. That is the only honest way to know whether live tracking was
good enough on a given day.

---

## Rigs YAML

Following `CameraSpec`'s existing convention — code holds the defaults, the YAML carries
only what a rig differs on.

```yaml
rigs:
  - name: "Rig 3"
    camera:
      serial: "24174008"
      fps: 100
      mode: video
      frame_out: true            # share frames for live pose

    pose_tracking:
      # Port pixel coordinates, full-frame space.
      # INDEX CONVENTION: index 0 is LED_1/SENSOR1 ... index 5 is LED_6/SENSOR6.
      # Looked up as int(correct_port) - 1. Getting this off by one rotates every
      # computed angle by ~60 degrees; it has happened once already.
      port_coordinates:
        - [840, 100]   # LED_1 / SENSOR1
        - [375, 100]   # LED_2 / SENSOR2
        - [160, 520]   # LED_3 / SENSOR3
        - [410, 920]   # LED_4 / SENSOR4
        - [860, 880]   # LED_5 / SENSOR5
        - [1110, 490]  # LED_6 / SENSOR6 (East; cue unused on visual trials)
```

and the model, global rather than per-rig since it is the same for a cohort:

```yaml
pose_tracking:
  model: "Y:/srogers/Behaviour/DEEPLABCUT_models/250822_wildtype_chemo_model_superanimal/project_folders/no_implant_superanimal-StefanRC-2025-08-22"
  snapshot: "snapshot-200.pt"      # omit for the latest
  warmup_images: "Y:/srogers/Behaviour/DEEPLABCUT_models/_reference_frames"
  crop_size: 640
  min_likelihood: 0.6
```

A `PoseTrackingSpec` dataclass in `rig_config.py` beside `CameraSpec` and `LaserSpec`,
same `from_dict` shape.

The full `new_rig_coords` table to transcribe, from `Session_nwb.py:549`:

| | Rig 1 | Rig 2 | Rig 3 | Rig 4 |
|---|---|---|---|---|
| LED_1 | 875, 55 | 860, 50 | 840, 100 | 875, 55 |
| LED_2 | 400, 50 | 370, 70 | 375, 100 | 400, 50 |
| LED_3 | 175, 475 | 150, 500 | 160, 520 | 175, 450 |
| LED_4 | 420, 890 | 400, 900 | 410, 920 | 400, 880 |
| LED_5 | 890, 890 | 870, 870 | 860, 880 | 875, 900 |
| LED_6 | 1130, 470 | 1110, 460 | 1110, 490 | 1125, 500 |

The two older "Red rig" coordinate sets also exist in that function. They stay in the
analysis library for old data and do not need to come across unless a red rig is still
running.

The source comment calls these "placeholder values ... should be updated via
calibration". They are not. Two independent checks say otherwise: the off-by-one note
records them being verified against DLC head position at port touch on every port of
every new rig, and drawing the rig 3 table over real footage puts every marker on its
port fitting, in a hexagon of radius 466 px +/- 4% with spokes exactly 60 degrees apart,
centred within 15 px of the frame centre. That is a calibration, not a guess. Treat them
as real, and re-verify with `calibrate_port_coordinates` if a camera is ever moved.

---

## Latency budget at 100 fps - measured

End to end through the real server and client, on real mouse footage at the 640 crop,
**with the GPU clocks locked** (see below -- this is not optional):

| pacing | requests | round trip | of which model |
|---|---|---|---|
| 20 Hz sustained | 300 | median 9.2 ms, **p95 10.7 ms**, worst 11.3 | 8.7 ms |
| 1 Hz, like trials | 40 | median 11.7 ms, **p95 12.7 ms**, worst 15.6 | 10.0 ms |

100% of requests returned a reading. **Comfortably inside the 30 ms target**, before
the rig's own few milliseconds of command round trip.

The frame-out half, measured separately against the camera at 100 fps: copying the
newest frame out of shared memory costs **median 0.54 ms, worst 1.82 ms**, and the
recorder dropped nothing (0 in transit, 0 writer-behind, ring peak 71 of 819).

### The GPU clocks are the whole ball game

This was the largest single effect found anywhere in this work, and it is pure
configuration. An idle GPU drops its graphics clock to ~210 MHz and its memory clock to
~810 MHz, and is slow enough to come back up that an occasional inference pays most of
the cost. Measured on the RTX 4000 Ada, varying only the gap between requests:

| gap | default clocks | clocks locked |
|---|---|---|
| back to back | 8.1 ms | 8.1 ms |
| 50 ms | 10.2 ms | 9.7 ms |
| 100 ms | 39.2 ms | 10.0 ms |
| 200 ms | 46.1 ms | 10.1 ms |
| 500 ms | 79.0 ms | 10.3 ms |
| **1 s** | **94.2 ms** | **10.1 ms** |

A protocol asks for a heading once or twice a trial. That is the right-hand end of this
table -- the worst case by default and the best case by configuration. Unlocked, the
system misses even the 100 ms ceiling; locked, it is flat at 10 ms at every rate.

Both clocks matter and locking only one is not enough: with graphics locked but memory
still idling it was still 25 ms at a one second gap, because HRNet is memory-bound.

`scripts/configure_rig.ps1` reports the clock state and locks both with `-Apply`
(Administrator, and it does not survive a reboot). The pose server checks at startup
and says so loudly if they are idling, and falls back to a much more aggressive
keepalive -- which works, but spends a fifth of the GPU doing nothing useful.

### Two other things worth knowing

**The cold first inference is 320-1300 ms**, against 8-10 ms warm, because CUDA is
choosing convolution algorithms and capturing the graph. Without the warmup that lands
on the first trial of a session.

**Frame age adds up to one frame period.** A request lands at a uniformly random point
in the frame period, so the newest frame is on average half a period old. At 100 fps
that is 0-10 ms; at 60 fps it would be 0-17 ms. A good reason for the higher frame rate
quite apart from the data.

## Benchmarking

Pull frames from `Y:\dwelch\Behaviour\2604_audiospatial` — confirmed present, plenty of
long sessions with mice in them.

What to measure, in order:

1. ~~**Inference time alone**, and at what crop.~~ **Answered**: see finding 1. CUDA
   graphs were the thing that mattered; 640 crop at 8.1 ms.
2. ~~**Agreement with the offline pipeline.**~~ **Answered, and this was the gate.**
   `tests/test_head_angle.py` imports the real `Session_nwb.find_angles`, binds it to a
   stub carrying only the attributes it reads, and runs both implementations over 620
   randomised poses. They agree on every case, across all three correction paths. The
   ear-swap check matters more than the agreement: swapping the two ear labels is
   exactly the mistake DeepLabCut makes, and the corrected bearing came back identical
   on 300/300 swapped poses.
3. ~~**Crop robustness.**~~ **Answered** for recorded footage: 100% detection at 640
   with the mouse up to 158 px off centre. Still worth re-checking on a rig, where the
   crop is fixed at the scales rather than placed around a known mouse.
4. **Keypoint quality at the new frame rate.** Old footage is 30 fps with a longer
   exposure; at 100 fps the exposure is necessarily shorter and the images are darker
   and noisier. The model was trained on the old look. This is a real risk to
   accuracy and it cannot be measured from archive footage — it needs a short test
   recording at 100 fps on a rig. Worth doing early, because the answer might be
   "retrain on 100 fps frames", which has a long lead time.

Point 4 is the one I would flag hardest. Everything else here is engineering with
predictable outcomes; whether the existing model generalises to shorter-exposure frames
is an empirical question with a potentially inconvenient answer.

---

## Order of work

Steps 1-7 are done. What is left needs a rig.

1. ~~Settle the `dlclive` / PyTorch question.~~ **Done, and the answer was no.**
   DeepLabCut-Live's latest release is 1.1.0, from the TensorFlow era, and will not load
   a DLC 3 PyTorch snapshot; it is not installable on Python 3.11 at all. DeepLabCut 3's
   own inference API does the job and is what the engine uses.
2. ~~Benchmark inference.~~ **Done**, and it rewrote finding 1: CUDA graphs, not
   cropping, were the thing that mattered. 640 crop at 8.1 ms.
3. ~~Frame out.~~ **Done.** 0.59 ms a frame, nothing dropped.
4. ~~The DLC server~~, with the warmup and the logging. **Done**, driveable from the
   command line, with a replay mode so a protocol can be tested without a rig.
5. ~~Validate against the offline pipeline.~~ **Done, and this was the gate.** The
   heading calculation is proved identical to `Session_nwb.find_angles` over 620
   randomised poses covering all three correction paths, and the ear-swap flip
   correction recovers the same bearing on 300/300 deliberately swapped poses.
6. ~~The peripheral, the `pose=` context, and the YAML spec.~~ **Done.**
7. ~~A test protocol.~~ **Done**: `pose_gated_cue.py` gated 12/12 trials end to end
   against a live server on real footage, and runs unchanged with pose tracking off.
8. **Run it on a rig with a mouse.** Everything above used recorded footage or the bench
   camera. What cannot be checked without a rig: whether the port coordinates are right
   for that camera's framing, whether the mouse really is inside a 640 centre crop when
   it is on the scales, and whether the round trip holds up with the behaviour system
   also running.
9. **Check keypoint quality at 100 fps.** This is the one that could still cost
   something. The model was trained on 30 fps footage; at 100 fps the exposure is
   necessarily shorter and frames are darker and noisier. Archive video cannot answer
   it -- it needs a short test recording on a rig. Worth doing early, because the answer
   might be "retrain on 100 fps frames", which has a long lead time.
10. **Verify each rig's port coordinates** by drawing them over a real frame from that
    rig. `scripts/pose_warmup_check.py --ports` does this, and it takes one look.
    Checked on the audiospatial footage, the rig 3 table lands squarely on the port
    fittings and forms a clean hexagon - radius 466 px +/- 4%, spokes exactly 60 degrees
    apart, centred within 15 px of the frame centre. So these are real calibration, not
    the placeholders the source comment calls them. Worth re-checking per rig anyway,
    because a nudged camera is silent: it shows up months later as a systematic offset
    in the analysis and nowhere before that.

Steps 9 and 10 are the two that could still change something. Everything else is
finished and measured.

## Phase 2 - the snapshot API and the live viewer

**Built.** `screenshots_temp/30_pose_viewer_real.png` is a screenshot of the actual
window running against real mouse footage - not a mockup. The three earlier mockups
(`20_dlc_live_window.png`, `21_dlc_live_flip.png`, `22_dlc_live_miss.png`) are kept
because they show the flip-corrected and failed states, which are awkward to stage on
demand.

### The API: one snapshot, everything derived from it

The current `heading(target_port=...)` asks about one port at a time. Port angles should
come *with* the snapshot instead, so a protocol can choose a port by looking at the
geometry rather than guessing one and asking.

```python
snap = self.pose.snapshot()
```

One inference, so the heading, every port angle and the position are mutually consistent
and share one `frame_id`:

```python
snap.ok                 # False means do not act on it; .reason says why
snap.heading            # 189.1 degrees, 0-360
snap.position           # (560, 513) px - the eyes-offset point angles are measured from
snap.frame_id           # joins to the recorded video and the DAQ

snap.port_angles        # {1: -133.2, 2: -74.9, 3: -8.1, 4: +60.6, 5: +120.2, 6: +173.3}
snap.port_distances     # {1: 712, 2: 436, 3: 400, ...} px
```

with the helpers that keep protocols readable:

```python
snap.angle_to(4)             # +60.6
snap.facing(4, within=30)    # False - and False when not ok, so no guard needed
snap.port_ahead(within=60)   # 3, the most aligned port, or None
snap.ports_by_angle()        # [(3, -8.1), (4, +60.6), (2, -74.9), ...]
snap.closest_port()          # 3, by distance
```

**"Nearest" is banned as a name.** Two different questions hide under it: which port the
mouse is *looking at* (`port_ahead`, by angle) and which it is *standing next to*
(`closest_port`, by distance). They agree often enough to hide a bug for months.

`heading()` is replaced rather than kept alongside. Nothing is in production yet, and
two ways to ask the same question is how they drift apart.

### Position in the rig

Three forms, cheapest first:

| | |
|---|---|
| `snap.position` | (x, y) px in the full frame - always available |
| `snap.distance_from_centre` | px - "is it on the scales?" |
| `snap.position_mm` | optional; needs one number per rig |

For millimetres no new calibration is needed. The configured port coordinates already
describe a hexagon of known pixel radius - 466 px for rig 3 - so adding the real
port-circle radius in mm to `pose_tracking:` makes mm-per-pixel fall out of a
calibration that already exists.

### The viewer shows only what the image told it

An earlier draft of this plan had the protocol annotating each snapshot with the port
it went on to cue, so the window could display it. That is dropped, deliberately.

It would have meant a bookkeeping call in every protocol, a second thing to keep in
step with the first, and a pose log that was half perception and half hearsay. The
pose system interprets images and returns what it found; it is told nothing and has no
channel to be told anything. So the window shows the heading, the position, and the
angle and distance to every port - all derived from the frame - and does not show which
port was cued, because it does not know and should not.

Decisions and outcomes belong in the protocol's own trial record. Every snapshot
carries a ``frame_id``, which is also in the video and the DAQ, so the two join
afterwards without either having to know about the other.

For the record, the rejected design had three tiers, by when the information exists:
context passed into the `snapshot()` call for what the protocol already knew; a
fire-and-forget `note()` afterwards for what it worked out from the snapshot; and
nothing at all for the outcome seconds later, which would have meant holding a CSV row
open and conflating a perception record with a behaviour one.

It would have worked. It was dropped because every one of those tiers is a line a
protocol author has to write and keep correct, in return for a column the analysis can
reconstruct by joining on `frame_id` anyway. The pose system is easier to use and
easier to trust when the only thing it can tell you is what it saw.

**When the row is written.** Each row is held briefly - until the next snapshot or about
2 s, whichever comes first - then written by the log thread that already exists. That
absorbs tier 2 without delaying anything. No note, and the row goes out with those
columns blank, which is also what happens if the protocol crashes mid-trial.

**Columns**: fixed ones for `trial`, `phase`, `target_port`, `cue_port`, `decision`,
plus a free-form `context` JSON column, so adding a field later does not mean changing
the schema.

None of that exists. The simplicity of not having it is worth more than the column it
would have filled in.

### The viewer, and how it gets frames

One window per rig, launched by the peripheral beside the server, following
`daq_view_subprocess.py`. DearPyGui, to match the rest of hexcontrol.

The request path must stay clean, so nothing is drawn or encoded on it:

1. On each snapshot the server stashes the raw crop - a ~0.4 MB memcpy, about 0.1 ms -
   and the keypoints into a small ring.
2. The **log thread**, already off the hot path, draws the overlay and encodes a JPEG.
3. The viewer polls `{"cmd": "recent", "since": n}` at about 5 Hz and gets the metadata
   plus the latest JPEG, roughly 40 KB.

Nothing is encoded unless a viewer has polled within the last few seconds, so a session
with no window open pays only the memcpy.

**Worth doing and nearly free:** write each snapshot's JPEG into the session folder. At
~40 KB and a couple of hundred decisions a session that is under 10 MB, and it means the
exact image behind any decision can be looked at months later - which the CSV alone
cannot give.

### The health bar

A strip along the bottom, sampled at about 2 Hz. NVML through `nvidia-ml-py` costs
**0.061 ms for a full sample**, measured, so this is free. (`torch.cuda.utilization()`
and friends are thin wrappers over the same library and raise without it installed.)

| field | why it is there |
|---|---|
| GPU utilisation, VRAM | ordinary health |
| temperature, power / limit | ordinary health |
| SM clock / max, **locked or idling** | the single biggest latency factor found anywhere here |
| memory clock / max, locked or idling | locking only the graphics clock is not enough |
| throttle reasons, decoded | `GpuIdle` is what cost 94 ms, silently |
| **NVENC sessions and fps** | the recorder encodes HEVC on this same GPU |
| pose median latency, % found | whether the thing is actually working |

Two of these earn their place from what this work turned up. The clock and throttle
fields would have caught the idle-downclock problem on the first session instead of
after a day of measurement. And NVENC runs on the same card as the inference, so if the
encoder and the model ever contend, this is the only place it would show.

### What was built, and where

| | |
|---|---|
| `pose_client.py` | `Reading` becomes `Snapshot`; port angles and distances always present |
| `pose_server.py` | all port angles and distances; snapshot ring; `recent` command; monitor snapshots; JPEG saving |
| `pose_viewer.py` (new) | the DearPyGui window |
| `pose_tracking.py` | launch a viewer per rig; `show_viewer` config |
| `rig_config.py` | `show_viewer`, `save_snapshots`, `arena_radius_mm` |
| `gpu_stats.py` (new) | NVML sampling for the health bar |
| `pose_gated_cue.py` | rewritten against `snapshot()` |
| new dependency | `nvidia-ml-py`, pure Python, in the server environment only |

### Both open questions, settled

**Live-only.** The latest snapshot plus a history table. Scrolling a whole session would
mean holding every JPEG in the viewer, and the saved snapshots already cover looking
back.

**The viewer does trigger its own snapshots**, at 2 Hz, so the window is not frozen on
the last decision between trials. They are marked ``monitor`` and are deliberately kept
out of the CSV: the log is a record of what the protocol asked for, and padding it with
display frames would make the request count meaningless. The history table shows only
protocol requests; the image panel shows whichever is newest and says which kind it is.

None of it runs unless a window is open. The ``recent`` poll is what marks a viewer as
attached, and with nothing attached the server does no drawing, no encoding and no
background snapshots at all.

---

## What is where

| | |
|---|---|
| `src/frame_out.{h,cpp}` | publishes the newest frame to shared memory |
| `python/frame_out_reader.py` | the reader; mirrors the header, version-checked |
| `python/head_angle.py` | the heading calculation, identical to offline |
| `python/pose_engine.py` | model loading, CUDA graph, crop, inference |
| `python/pose_warmup.py` | the startup self-test |
| `python/pose_overlay.py` | drawing keypoints, heading and ports |
| `python/pose_server.py` | one process per rig; serves headings, logs them |
| `python/pose_client.py` | what a protocol uses; standard library only |
| `tests/test_head_angle.py` | the equivalence gate against hex_behav_analysis |
| `scripts/benchmark_pose.py` | the crop-size and timing sweep |
| `scripts/test_frame_out.py` | checks the shared memory block |
| `scripts/test_pose_live.py` | the end-to-end round trip |
| `scripts/pose_warmup_check.py` | run the warmup on its own |
| `hexcontrol/core/peripherals/pose_tracking.py` | the peripheral |
| `hexcontrol/protocols/pose_gated_cue.py` | the worked example protocol |

The environment is the conda env `dlclive`: torch 2.6.0+cu124 and deeplabcut 3.0.2.
hexcontrol does not need it -- only the server does, and the peripheral launches it by
path.
