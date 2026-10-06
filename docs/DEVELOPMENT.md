# Setting up a development machine

How to get from a fresh clone to a working build and a verified camera, on a new
Windows machine. Follow it top to bottom; the whole thing is about 30 minutes of
work plus a long unattended first build.

Everything here was checked on Windows 11 with Visual Studio 2026 Community
(MSVC 14.50.35717), CMake 4.2.3 as bundled with Visual Studio, and Spinnaker
4.3.0.189.

---

## 1. Install the prerequisites

### Visual Studio (required, even if you plan to work in VS Code)

VS Code does not ship a C++ compiler. The toolchain, the Windows SDK, CMake and
Ninja all come from Visual Studio, and the build scripts find them automatically.

Install **Visual Studio 2022 or newer** — Community edition is fine — and in the
installer tick the **Desktop development with C++** workload. Make sure these
individual components are included (the workload selects them by default):

- MSVC v143 or newer, x64/x86 build tools
- Windows 11 SDK
- **C++ CMake tools for Windows** — this is what provides CMake and Ninja

You do not need to add anything to `PATH`. `scripts\dev_env.cmd` locates Visual
Studio with `vswhere` and sets the environment up per build.

### Teledyne Spinnaker SDK (required)

Download the **SDK** — not the runtime-only package — from Teledyne's site and
install it to the default location:

```
C:\Program Files\Teledyne\Spinnaker
```

Install elsewhere and you will need to tell CMake where it is (see
[Troubleshooting](#spinnaker-sdk-not-found)). During installation, accept the USB
driver when prompted; without it no camera will be detected.

Afterwards you should have all three of these:

| Path | Why it is needed |
|---|---|
| `include\Spinnaker.h` | headers to compile against |
| `lib64\vs2015\Spinnaker_v140.lib` | import library to link against |
| `bin64\vs2015\Spinnaker_v140.dll` | runtime, put on `PATH` by the installer |

The `vs2015` folder is correct even on much newer compilers: it holds the v140
libraries, and MSVC v140 through v14x are binary compatible. The `vs2017` folder
contains only the GPU extras, which this project does not use.

### Git (required)

Any recent version. The clone is around 60 MB because vcpkg is vendored into the
repository rather than used as a submodule, so there is nothing extra to initialise.

### VS Code (recommended)

Install VS Code, then the two extensions below. The repository recommends them, so
VS Code will offer to install them when you first open the folder:

- **CMake Tools** (`ms-vscode.cmake-tools`)
- **C/C++** (`ms-vscode.cpptools`)

### Python 3 (optional)

Only used by `scripts\check_recording.py`, which verifies a recorded session. Any
Python 3 on `PATH` will do — it uses the standard library only, so no virtualenv or
packages are needed. Skip it and the test script simply skips that final check.

---

## 2. Clone

```sh
git clone <repository-url> C:\dev\projects\behaviour_camera
cd C:\dev\projects\behaviour_camera
```

Avoid paths with spaces or non-ASCII characters. Some vcpkg port build scripts
handle them poorly.

---

## 3. First build

From Explorer, double-click **`build.cmd`**. From a terminal:

```sh
build.cmd
```

**The first build takes a long time and needs an internet connection.** Two things
happen only once:

1. **vcpkg bootstraps itself.** `vcpkg.exe` is excluded by vcpkg's own `.gitignore`,
   so a fresh clone does not contain it; the CMake toolchain downloads it.
2. **Dependencies are installed.** `vcpkg.json` asks for OpenCV, GLFW and
   nlohmann-json, but OpenCV's default features drag in 26 packages altogether,
   including protobuf, abseil, flatbuffers, libjpeg-turbo, libpng, libwebp and tiff.
   On a machine with no vcpkg binary cache these are **compiled from source**.

Budget an hour and leave it running. On a machine whose vcpkg binary cache is
already populated the same step took about seven minutes. Afterwards, configuring
takes about five seconds and an incremental build a few seconds, so this cost is
paid once per machine. [Speeding this up for additional machines](#8-speeding-up-setup-on-more-machines)
is worth reading if you are commissioning several rigs.

When it finishes you should see:

```
Build succeeded.
Binaries: out\build\ninja-release\
```

with `behaviour_camera.exe` and `camera_probe.exe` in that folder.

---

## 4. Set up VS Code

Open the folder:

```sh
code .
```

Install the recommended extensions when prompted. Then:

1. CMake Tools reads `CMakePresets.json` and configures automatically. If it asks
   you to choose a configure preset, pick **Ninja x64 Release**.
2. If it ever asks you to select a *kit*, something is wrong — this project uses
   presets, not kits. Run **CMake: Delete Cache and Reconfigure** from the command
   palette.

What you get:

| Action | How |
|---|---|
| Build | **Ctrl+Shift+B** |
| Debug | **F5**, then pick a configuration |
| Switch Release/Debug | the preset name in the status bar |
| Run a task | **Ctrl+Shift+P** → *Tasks: Run Task* |

Three debug configurations are set up: probe the camera, record a test session into
`out\test_recordings\`, and run whichever target is selected in the status bar.
IntelliSense gets its include paths and flags from CMake Tools, so there is no
`c_cpp_properties.json` to maintain.

---

## 5. Check that it works

Three checks, each one doing a bit more than the last.

### Does it see a camera?

```sh
test_camera.cmd list
```

```
Spinnaker 4.3.0.189
1 camera detected
  [0] serial 26043809, Blackfly S BFS-U3-13Y3M, link SuperSpeed
```

`link SuperSpeed` means USB 3. If it says `HighSpeed` you are on a USB 2 port or
cable, and the camera will not reach full frame rate.

### Can it capture at rate?

```sh
test_camera.cmd probe
```

This prints what the camera reports about itself, then grabs 600 frames and measures
what it achieved. Nothing is written to disk, which is the point: if frames are lost
here, the cause is the camera, the cable or the USB link, and not the recorder. A
healthy result ends with:

```
  dropped (frame-ID gaps)   0
  measured rate             60.04 fps (asked for 59.99)
  Clean: every frame the camera produced arrived intact.
```

Use `-Serial <id>` if more than one camera is attached.

### Can it record and produce valid files?

```sh
test_camera.cmd record
```

Records about ten seconds into `out\test_recordings\`, stops itself with a signal
file, then verifies the output: that the binary holds exactly as many whole frames
as the metadata claims, that frame IDs increase strictly, and how many frames were
dropped. It should end with `TEST PASSED.`

Test recordings are **not** deleted automatically, and run about 80 MB per second at
60 fps. Clear out `out\test_recordings\` when you are done.

---

## 6. How the build is wired together

Worth five minutes if you are going to change it.

**`CMakePresets.json` is the single source of truth.** The command line, VS Code and
Visual Studio all read it, so they cannot drift apart. Three presets:

| Preset | For |
|---|---|
| `ninja-release` | day-to-day work. RelWithDebInfo: optimised, with debug symbols |
| `ninja-debug` | debugging. Debug CRT, links the debug Spinnaker library |
| `vs2026` | generates a `.sln` for Visual Studio's debugger and profiler |

Keeping both Ninja and Visual Studio is deliberate. Ninja builds incrementally much
faster, but Visual Studio still has the better debugger and the only profiler —
useful once there are multiple threads to inspect. `build.cmd vs` generates the
solution; both read the same presets, so you can switch freely.

All presets share one dependency tree via `VCPKG_INSTALLED_DIR` pointing at
`vcpkg_installed/` in the source directory. Without that, each preset would build
its own copy of OpenCV.

**Spinnaker is found, not assumed.** `CMakeLists.txt` uses `find_path`/`find_library`
under `SPINNAKER_ROOT` and fails with a useful message if the SDK is missing. It
defines release *and* debug import libraries, so a Debug build links
`Spinnakerd_v140.lib` rather than mixing the release SDK with the debug CRT.

**DLLs are copied next to the executables** after each build via
`$<TARGET_RUNTIME_DLLS>`, so binaries run from the build tree without `PATH` games.
Spinnaker's own DLLs are not copied because its installer already puts them on the
system `PATH`.

**Two compiler flags are load-bearing**, both commented in `CMakeLists.txt`:

- `/EHsc` **must be present.** MSVC does not enable exception unwinding unless asked,
  and CMake 4.x no longer passes it by default. Without it, destructors are not
  guaranteed to run while an exception propagates — and in this program every camera
  fault arrives as a thrown `Spinnaker::Exception`.
- `/Zc:__cplusplus` **must be absent.** `SpinnakerPlatform.h` switches on
  `__cplusplus >= 201402L`, and that branch defines its deprecation macros as
  `enum [[deprecated(msg)]]` — an attribute position MSVC rejects outright. Enabling
  the flag breaks every file that includes `Spinnaker.h`. The C++ standard version is
  unaffected; that comes from `CMAKE_CXX_STANDARD`.

---

## 7. Troubleshooting

### `No CMAKE_CXX_COMPILER could be found`

You ran `cmake` directly from a shell with no MSVC environment. Use `build.cmd`,
which sets it up, or build from VS Code. If you want to run `cmake` by hand, do it
from **cmd** — the environment variables `dev_env.cmd` sets only persist in the
shell that ran it, and PowerShell will not inherit them:

```bat
scripts\dev_env.cmd && cmake --preset ninja-release
```

In PowerShell, wrap the pair in a single `cmd` invocation instead:

```powershell
cmd /c "scripts\dev_env.cmd && cmake --preset ninja-release"
```

### Spinnaker SDK not found

```
Could not find the Spinnaker SDK under 'C:/Program Files/Teledyne/Spinnaker'.
```

Either the SDK is not installed (the runtime-only package is not enough — you need
headers and `.lib` files), or it is somewhere else. For a different location, either
edit `SPINNAKER_ROOT` in `CMakePresets.json`, or override it per configure:

```sh
cmake --preset ninja-release -DSPINNAKER_ROOT="D:/path/to/Spinnaker"
```

### `error C3837: attributes are not allowed in this context`

Something added `/Zc:__cplusplus` to the compile flags. Remove it — see
[section 6](#6-how-the-build-is-wired-together).

### `warning C4530: C++ exception handler used, but unwind semantics are not enabled`

`/EHsc` has gone missing from the compile flags. Put it back; this one is not
cosmetic.

### `0 cameras detected`

In rough order of likelihood:

1. Another program has the camera open — SpinView, or a recording still running.
   Only one process can hold a camera at a time.
2. The Spinnaker USB driver was not installed. Re-run the SDK installer and accept
   the driver, then check Device Manager for a `PGRDevices` entry.
3. Bad cable, or a USB 2 port. Over 3 m you need an active cable.
4. The camera needs a moment after being plugged in. Try again.

### Build succeeds but the exe fails to start, or complains about a missing DLL

OpenCV and GLFW DLLs are copied next to the executable automatically, so this
usually means Spinnaker's DLLs are not on `PATH` — normally the installer puts
`bin64\vs2015` there. Check with:

```sh
where Spinnaker_v140.dll
```

If that finds nothing, add `C:\Program Files\Teledyne\Spinnaker\bin64\vs2015` to
`PATH` and open a new terminal, or configure with
`-DINSTALL_SPINNAKER_RUNTIME=ON` and run `cmake --install` for a self-contained copy.

### `cannot be loaded because running scripts is disabled on this system`

You ran `scripts\test_camera.ps1` directly. Use `test_camera.cmd`, which passes
`-ExecutionPolicy Bypass`, or run it as:

```sh
powershell -ExecutionPolicy Bypass -File scripts\test_camera.ps1
```

### vcpkg wants to rebuild everything after I changed something

vcpkg keys its cache on a hash that includes the compiler version, so a Visual
Studio update triggers a rebuild. That is expected. A rebuild after changing presets
usually means `VCPKG_INSTALLED_DIR` is no longer shared — check it still points at
`${sourceDir}/vcpkg_installed`.

### Generator mismatch after switching presets

```sh
build.cmd clean
```

Or delete `out\build\<preset>` and configure again.

---

## 8. Speeding up setup on more machines

If you are setting up several rig computers, do not pay the dependency build cost
on each one. vcpkg can share compiled packages through a binary cache on a network
path:

```sh
setx VCPKG_BINARY_SOURCES "clear;files,\\server\share\vcpkg-cache,readwrite"
```

The first machine populates it; the rest extract in minutes instead of compiling.
For reference, the cache on an already-set-up machine here is about 1.2 GB.

There is also a worthwhile opportunity nobody has taken yet: **this project uses
OpenCV for exactly three lines** — one `cv::Mat` wrapper and one `cv::resize` in the
preview path. Those pull in all 26 packages. Disabling OpenCV's default features in
`vcpkg.json`, or dropping OpenCV entirely once the preview scales on the GPU, would
cut first-build time dramatically and shrink deployments. Left alone for now because
it needs the preview code changed at the same time.

---

## 9. If this is a rig machine, not just a dev box

Building is only part of commissioning a recording computer. Three settings matter
for capture reliability and have nothing to do with the compiler:

- **Power plan.** Switch off *Balanced* to **High performance**. Balanced parks cores,
  ramps frequency lazily, and leaves USB selective suspend enabled — a known cause
  of intermittent USB3 camera dropouts.
- **Antivirus exclusion.** Exclude the capture output directory from real-time
  scanning. Scanning a sustained write stream causes latency spikes, and a spike
  costs frames.
- **A dedicated capture drive.** Keep recordings off the OS disk. Frames are written
  synchronously with very little buffering, so anything else competing for the drive
  shows up as dropped frames. Watch free space too: these drives lose a lot of write
  speed as they fill, and dropped frames get noticeably worse with them.

One camera at 1280×1024 Mono8 needs about 39 MB/s at 30 fps or 79 MB/s at 60 fps,
and roughly 141 GB per hour at 30 fps. Size the drive from the longest session you
intend to run, times the number of cameras.

See `README.md` for the known rough edges in the recorder itself.
