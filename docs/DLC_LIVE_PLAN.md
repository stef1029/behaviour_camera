# DLC Live — plan of action

Live pose estimation during a session, so a protocol can decide what to do based on
where the mouse is looking. Head angle is the primary quantity; inference runs on
demand at specific moments, not continuously.

This document is the plan. Nothing here is built yet except where it says so.

**Targets:** 30 ms from "protocol asks" to "protocol has an angle". 100 ms is the
ceiling. Cameras run at 60–120 fps, most likely 100.

---

## What was checked, and what it changed

Four findings from reading the existing code and the model files. Two of them change
the plan from what we discussed before.

### 1. The model is HRNet-w32, not ResNet-50 — this is the main risk

`config.yaml` says `default_net_type: resnet_50`, but that is only the project
default. The shuffle that was actually trained is in
`dlc-models-pytorch/iteration-0/.../train/pytorch_config.yaml`:

```yaml
net_type: hrnet_w32
```

HRNet-w32 is more accurate than ResNet-50 and considerably more expensive, and DLC
runs it at full frame resolution (padded to a multiple of 32 — 1280×1024 already is,
exactly). The 448×448 in that config is the *training* crop, not the inference size.

This machine has an **RTX 4000 Ada (20 GB)**, roughly a third of a 4090's compute. A
rough estimate for HRNet-w32 over 1.31 Mpx is ~190 GFLOPs, and HRNet's multi-resolution
fusion is memory-bound rather than compute-bound, so the realistic range is **40–80 ms
per full-frame inference**. That misses the 30 ms target and crowds the 100 ms ceiling
before any of the other costs are counted.

So full-frame inference is not the design. **Region of interest cropping is essential,
not an optimisation** — see the latency budget below. Cropping a 384×384 window around
the mouse is 8.9× fewer pixels and brings inference to roughly 5–10 ms, which makes the
whole budget comfortable. 384 is divisible by 32, so no padding is wasted.

If ROI cropping somehow is not enough, in rough order of what to try: fp16/TF32
autocast (~2×), TensorRT export (~2–3× more), then retraining with `hrnet_w18` or
`resnet_50`. You offered to retrain a smaller model — hold that in reserve; it probably
will not be needed.

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
live code, and if anyone changes the sensor resolution or ROI-crops *before* computing
coordinates, both numbers and the port coordinates all break together. The live code
must compute coordinates in **full-frame pixel space** and map the ROI crop back, never
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
unmeasurable next to the 8.9× that ROI cropping buys.

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
│  shm → ROI crop → HRNet → keypoints │   separate process, separate GPU work
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
4. On request: grab the latest frame, ROI crop, infer, compute the heading with the
   algorithm above, reply.
5. Keepalive: one throwaway inference every ~500 ms when idle, so the GPU does not
   clock down between requests. Without this the first inference after a quiet period
   can take two or three times as long as the steady-state figure — which is exactly
   when it matters, because the quiet period is the inter-trial interval.
6. Log every inference to disk.

**ROI tracking.** Keep the previous detection's ear midpoint and crop 384×384 around
it. If the mean likelihood of the needed keypoints drops below a threshold, or nothing
has been inferred for a while, fall back to one full-frame inference to re-acquire. All
coordinates are mapped back to full-frame space before any angle is computed, since the
port coordinates and both pixel thresholds live in that space.

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
  roi_size: 384
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

Note the source comment calls these "placeholder values ... should be updated via
calibration", but the off-by-one note shows they were since verified against DLC head
position at port touch on every port of every new rig. Treat them as real, and
re-verify with `calibrate_port_coordinates` if a camera is ever moved.

---

## Latency budget at 100 fps

With ROI cropping, which finding 1 makes mandatory:

| Stage | Expected | Notes |
|---|---|---|
| Frame age when requested | 0–10 ms | uniform; the request lands mid-frame-period |
| Shared memory read | <1 ms | 1.31 MB memcpy |
| ROI crop, to tensor, to GPU | 1–2 ms | crop first, so only 147 kB uploads |
| **HRNet-w32 on 384×384** | **5–10 ms** | the figure to measure first |
| Heatmap decode + refine | 1–3 ms | |
| Heading, flip check, port angles | <0.5 ms | pure arithmetic |
| IPC back to the protocol | ~1 ms | local |
| **Subtotal** | **~10–28 ms** | |
| Rig command round trip | 2–5 ms | your figure, outside our control |
| **Total** | **~12–33 ms** | |

That lands on the 30 ms target with little margin, and well inside the 100 ms ceiling.
Full-frame inference instead would put the subtotal at 45–90 ms — inside the ceiling,
missing the target. Hence ROI.

Two honest caveats. The 5–10 ms inference figure is an estimate for HRNet-w32 on an
RTX 4000 Ada and **must be measured before anything is built on it** — it is the one
number the whole budget rests on. And the frame-age term means the *worst case* is
about a frame period worse than the average; running at 100 rather than 60 fps cuts
that term from 0–17 ms to 0–10 ms, which is a good reason for the higher frame rate
quite apart from the data.

---

## Benchmarking

Pull frames from `Y:\dwelch\Behaviour\2604_audiospatial` — confirmed present, plenty of
long sessions with mice in them.

What to measure, in order:

1. **Inference time alone**, full-frame vs 384×384 ROI, fp32 vs autocast. This decides
   whether the plan above holds. Do this before writing any of Part A.
2. **Agreement with the offline pipeline.** Run the live path over frames from a session
   that has already been through full post-processing, and compare bearings. They should
   match to within floating-point noise, since it is the same algorithm. Any systematic
   difference — especially a 180° cluster — is a bug in the flip correction's
   transcription.
3. **ROI tracking robustness.** How often does the ROI lose the mouse and need a
   full-frame re-acquire? Fast turns and rears are the cases to watch.
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

1. **Settle the `dlclive` / PyTorch question.** Half-hour experiment. Decides Part B's shape.
2. **Benchmark inference** on audiospatial frames, full-frame vs ROI. Decides whether the
   budget holds. Nothing else should start before these two.
3. **Part A — frame out.** Self-contained C++ in this repo, testable with a small Python
   script that attaches and saves a PNG. Ready to build now.
4. **Part B — the DLC server**, with the warmup and the logging, driven from the command
   line first so it can be developed without hexcontrol in the loop.
5. **Validate against the offline pipeline** (benchmark point 2) before wiring anything
   into a protocol. This is the gate: if live and offline disagree, nothing downstream
   is trustworthy.
6. **Part C — the peripheral**, the `pose=` context, and the YAML spec.
7. **A test protocol** that does nothing but log headings, to run on a real rig before
   any experiment depends on it.
8. **A 100 fps test recording** and a check on keypoint quality (benchmark point 4) — in
   parallel with the above, since a bad answer means retraining.

Steps 1 and 2 are the ones that could change this plan. Everything after step 3 is
ordinary work.
