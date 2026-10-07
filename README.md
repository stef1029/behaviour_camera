# Behaviour Camera

High-speed camera capture for the behaviour rigs, using the Teledyne Spinnaker SDK,
GLFW and Dear ImGui. Records raw Mono8 frames to a flat binary file alongside a JSON
metadata sidecar and a frame-ID log; `hex_behav`'s post-processing converts the
binary to video afterwards.

## Quick start

Two double-clickable scripts, no terminal needed:

| | |
|---|---|
| `build.cmd` | Build everything |
| `test_camera.cmd` | Build, then check the camera works |

From a terminal they take arguments:

```sh
build.cmd                 # Release (RelWithDebInfo: optimised, with symbols)
build.cmd debug           # Debug
build.cmd vs              # generate a Visual Studio solution instead
build.cmd clean           # wipe the build tree and rebuild

test_camera.cmd list      # which cameras can I see?
test_camera.cmd probe     # measure the capture rate (writes nothing)
test_camera.cmd record    # also record a short session and verify the files
test_camera.cmd record -Seconds 30 -Fps 120 -Serial 22181614
```

Test recordings go to `out/test_recordings/`, never to a data drive. They are not
deleted automatically — about 80 MB per second recorded at 60 fps, so clear that
folder out when you are done.

## Prerequisites

Setting up a new machine from scratch? See **[docs/DEVELOPMENT.md](docs/DEVELOPMENT.md)**
for step-by-step instructions, troubleshooting, and notes on commissioning a rig
computer. The short version:

- **Visual Studio 2022 or newer** with the *Desktop development with C++* workload.
  The build finds it automatically with `vswhere`, and uses the Ninja and CMake that
  ship with its CMake component, so nothing needs to be on your PATH.
- **Teledyne Spinnaker SDK**, installed to `C:/Program Files/Teledyne/Spinnaker`.
  Point `SPINNAKER_ROOT` elsewhere if yours is not there.
- **vcpkg**, included as a git submodule. Dependencies install on first configure.
- **ffmpeg** with NVENC, on PATH. Only needed for `--mode video`; a rig set to
  video without it fails when a session starts, not at build time.
- **Python 3** is optional, used only by the recording verifier.

From nothing to a built binary:

```sh
git clone --recurse-submodules https://github.com/stef1029/behaviour_camera.git
cd behaviour_camera
build.cmd
```

If you cloned without `--recurse-submodules`, the `vcpkg` directory will be empty and
CMake will not find its toolchain file. Fix it with:

```sh
git submodule update --init
```

## Working in VS Code

Install the recommended extensions when prompted (CMake Tools and C/C++), then:

- **Ctrl+Shift+B** builds.
- **F5** debugs. Three launch configurations are set up: probe the camera, record a
  test session, or run whichever target is selected in the status bar.
- The CMake Tools status bar switches between the `ninja-release`, `ninja-debug`
  and `vs2026` presets.

`CMakePresets.json` is the single source of truth, so the command line, VS Code and
Visual Studio all build identically. Visual Studio remains worth keeping for its
debugger and profiler, which are better than the VS Code equivalents for threading
and performance work; `build.cmd vs` generates the solution for that.

## Programs

### `behaviour_camera` — the recorder

```sh
out\build\ninja-release\behaviour_camera.exe --serial_number 26043809 --fps 60
```

| Argument | Description | Default |
|---|---|---|
| `--serial_number` | Camera serial number | required |
| `--id` | Mouse/subject ID | `NoID` |
| `--date` | Date/time string, used in filenames | now |
| `--path` | Output directory, created if missing | `E:\test_vid_output\{date}_{id}` |
| `--fps` | Frame rate; clamped to what the camera allows | `60` |
| `--windowWidth`, `--windowHeight` | Preview window size | `800` x `600` |
| `--mode` | `raw` or `video` | `raw` |
| `--ring-buffer-mb` | RAM held between capture and writing | `1024` |
| `--histogram` | Start with the exposure histogram showing | off |
| `--rig` | Rig name used in the signal filenames | from the serial |
| `--exposure-min` | Auto-exposure floor, microseconds | `4000` |
| `--stream-buffers` | Frames the driver may hold while writing | `300` |
| `--strobe-line` | GPIO line pulsed once per frame | `2` |
| `--qp`, `--gop` | Encoder quality and keyframe interval | `23`, `30` |
| `--no-preview` | Record without a preview window | preview on |
| `--frame-out` | Publish frames to shared memory for live pose | off |

Stops on **Esc** in the preview window, or when a file named
`stop_camera_<rig>.signal` appears in the output directory. On finishing it writes
`rig_<rig>_camera_finished.signal`.

### Recording modes

| Mode | Output | Per camera at 30 fps | Notes |
|---|---|---|---|
| `raw` | `*_binary_video.bin` | ~141 GB/hour | Nothing touched. The default. |
| `video` | `*_video.mkv` | ~1.4 GB/hour | HEVC on the GPU via ffmpeg + NVENC |

`video` needs ffmpeg on PATH and an NVIDIA card. Encoder settings — codec, quality,
GOP, container — live in the config file, so most sessions need no arguments; use
`--mode video` to switch a single run.

Measured on real behaviour footage: all-intra HEVC is about 20x smaller than raw
(against 15x for the MJPG the old post-processing produced), and GOP 30 — a keyframe
a second — is about 105x. Encoding also cuts the *write* rate by the same factor,
which matters because disk stalls are what cost frames.

Output per session:

| File | Contents |
|---|---|
| `*_binary_video.bin` or `*_video.mkv` | The frames |
| `*_Tracker_data.json` | Resolution, pixel format, frame rate, start/end time, frame IDs, drop counts, resolved settings |
| `*_frame_ids_backup.txt` | Frame IDs flushed during the session, so they survive a crash |
| `*_camera_log.txt` | Everything the recorder printed |

**Output frame *i* is the *i*-th entry of `frame_IDs`**, in either mode. The *value*
there identifies which frame the camera produced, and those values have gaps whenever
a frame is lost in transfer. Never use a frame ID as a file offset or a frame index.

The recorder checks this invariant before it exits — for raw by dividing the file
size, for video by counting packets with ffprobe — and records the result as
`output_frame_count`. A mismatch is reported and gives a non-zero exit code.

### `camera_probe` — read-only diagnostic

Enumerates cameras, prints what each reports about itself, and optionally measures
the rate actually achieved. It writes nothing, so it separates camera and USB
problems from disk problems: frames lost during a probe are not the recorder's fault.

```sh
out\build\ninja-release\camera_probe.exe                                  # list
out\build\ninja-release\camera_probe.exe --serial 26043809 --frames 600 --fps 60
```

Exit codes: 0 clean, 1 ran but lost frames, 2 error.

### `scripts/check_recording.py` — verify a session

```sh
python scripts\check_recording.py out\test_recordings\261006_161227_test
```

Checks the binary holds exactly as many whole frames as the metadata claims, that
frame IDs increase strictly, how many frames were dropped, and the rate achieved.
Exit codes: 0 consistent, 1 frames dropped but files sound, 2 inconsistent.

## Configuration

Every setting has a working default in the code, so the recorder needs no config
file and runs with only `--serial_number`. Anything a rig differs on is passed on
the command line, and the rig launcher fills those in from each rig's `camera:`
section in `rigs.yaml` — which makes that file the single place rig hardware is
described.

```yaml
rigs:
  - name: "Rig 3"
    camera:
      serial: "24174008"
      fps: 30
      mode: video              # "raw" | "video"
      exposure_min_us: 4000    # per-rig illumination
      window: { width: 640, height: 512 }
```

List only what the rig differs on; omitted values keep the built-in default. Run
`behaviour_camera --help` for the full set of overrides.

## Rig machine setup

```sh
powershell -ExecutionPolicy Bypass -File scripts\configure_rig.ps1
```

Reports the Windows settings that affect capture reliability — power plan, USB
selective suspend, antivirus exclusion, capture drive headroom — and changes nothing
unless given `-Apply` (as Administrator).

## How a session runs

Three threads, so nothing slow can stall the camera:

| Thread | Does |
|---|---|
| capture | grab, copy into a pooled buffer, hand the camera's buffer straight back |
| writer | drain the ring to disk or the encoder; owns the frame-ID record |
| main | the preview window, the stop checks, the disk guard |

Between capture and writing sits a ring of preallocated buffers — 1 GB by default,
about 800 frames or 13 seconds at 60 fps. That is the slack that absorbs a disk
stall. The camera's own stream buffers add another 5–10 seconds in front of it.

If the writer ever does fall behind far enough to exhaust the ring, frames are
dropped **deliberately and counted** as `dropped_no_buffer`, separately from
`dropped_frames` (lost in transit between camera and host). `ring_buffer_peak` is
recorded every session, so you can see how close you came even when nothing was
lost — it is the best early warning there is.

The preview is built around one question: is this recording healthy? It is laid
out as two thin banners with the picture between them, because the behaviour
system opens the window at 640x512 and a side panel leaves almost nothing for the
image at that width.

The top banner answers the question - green `RECORDING`, or red `FRAMES LOST`
with the count - in a form readable from across the room. The bottom carries the
numbers: frame rate against target, buffer occupancy and its peak, write rate,
disk time remaining, exposure, frame-interval p99, camera temperature. If the
window is too narrow for all of them it drops the least important first, and the
key hints go before any of the numbers do.

Two keys open overlays on top of the image, so no permanent space is spent on
them:

| Key | |
|---|---|
| `d` | traces of frame rate and buffer peak over the last minute, interval statistics, drop counts, output file |
| `h` | exposure histogram and the saturated-pixel fraction |
| `esc` | stop, after a confirmation |

The buffer trace plots the *peak* between samples rather than the instantaneous
value: the buffer fills and drains faster than the plot ticks, so sampling it
directly draws a flat line straight through a spike. Frame-interval median, p99
and worst are there because a long tail is a stall that nearly cost a frame,
which is the warning before one that does.

`--histogram` starts with the histogram already showing, for setting a camera up.

## Sharing frames with other programs

`--frame-out` publishes each captured frame to a named shared memory block, so
another process can read what the camera is seeing while the session records.
It is off by default and costs about 0.5 ms a frame when on.

The recording cannot suffer for it. The writer never waits for, checks for, or
can be blocked by a reader: the synchronisation is a seqlock over two slots,
which has no lock at all. The worst a misbehaving reader can do is notice it
read a torn frame and try again. Measured at 100 fps with a reader attached,
the recorder dropped nothing.

```sh
behaviour_camera.exe --serial_number 26043809 --rig rig3 --fps 100 --frame-out
```

The layout of the block is defined and documented in
[src/frame_out.h](src/frame_out.h), which carries a version field that readers
must check.

**Live pose estimation** is built on this, and lives in the **PoseLink** library
in `hex_behav_control` alongside BehavLink and ScalesLink — not here, so this
repository stays about running the camera. It reads the block, runs DeepLabCut
on demand and tells a protocol which way the mouse is facing. See
`hex_behav_control/PoseLink/README.md`.

A rig doing live pose also needs its GPU clocks locked, which is
`PoseLink/scripts/configure_gpu.ps1` — worth up to 10x on inference latency and
nothing to do with capture, which is why it is over there.

## Known rough edges

Recorded here so they are not rediscovered:

- `/Zc:__cplusplus` cannot be enabled: `SpinnakerPlatform.h` then expands its
  deprecation macros to `enum [[deprecated]]`, which MSVC rejects (C3837). See the
  comment in `CMakeLists.txt`.

## Deployment

```sh
cmake --install out/build/ninja-release
```

Installs the executables and the vcpkg DLLs to `out/install/`. Spinnaker's own DLLs
are *not* bundled, because its installer already puts them on the system PATH;
configure with `-DINSTALL_SPINNAKER_RUNTIME=ON` for a self-contained package, at the
cost of ~300 MB of mostly unused GUI DLLs.
