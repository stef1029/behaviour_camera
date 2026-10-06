# Behaviour Camera

High-speed camera capture for the behaviour rigs, using the Teledyne Spinnaker SDK,
OpenCV and GLFW. Records raw Mono8 frames to a flat binary file alongside a JSON
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
- **vcpkg**, vendored in this repository. Dependencies install on first configure.
- **Python 3** is optional, used only by the recording verifier.

## Working in VS Code

Install the recommended extensions when prompted (CMake Tools and C/C++), then:

- **Ctrl+Shift+B** builds.
- **F5** debugs. Three launch configurations are set up: probe the camera, record a
  test session, or run whichever target is selected in the status bar.
- The CMake Tools status bar switches between the `ninja-release`, `ninja-debug` and
  `vs2026` presets.

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

Stops on **Esc** in the preview window, or when a file named
`stop_camera_<rig>.signal` appears in the output directory. On finishing it writes
`rig_<rig>_camera_finished.signal`.

Output per session:

| File | Contents |
|---|---|
| `*_binary_video.bin` | Raw Mono8 frames, back to back, no header |
| `*_Tracker_data.json` | Resolution, pixel format, frame rate, start/end time, frame IDs |
| `*_frame_ids_backup.txt` | Frame IDs flushed during the session, so they survive a crash |

**Position in the `.bin` is the index that frame IDs refer to.** Frame *i* of the
file is the *i*-th entry of `frame_IDs`, whose value identifies which frame the
camera produced — and those values have gaps whenever a frame is lost in transfer.
Never use a frame ID as a file offset.

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

## Known rough edges

Recorded here so they are not rediscovered:

- **Rig names are still a table in `src/main.cpp`.** An unlisted camera works fine,
  it just gets called `cam_<serial>`. Moving this to a config file is the next job.
- **Only 10 stream buffers.** The camera's default gives roughly 170 ms of slack at
  60 fps, so any disk stall longer than that drops frames. Raising
  `StreamBufferCountManual` is the cheapest available reliability win.
- **Capture, disk writes and the preview all share one thread**, so a disk hiccup
  stalls capture directly.
- **One incomplete frame triggers a full camera re-initialisation** plus a 5 s
  cooldown, turning a routine dropped packet into seconds of lost recording — and
  resetting the camera's frame-ID counter, which breaks frame IDs as an index.
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
