# Behaviour Camera

High-speed camera capture application using Teledyne Spinnaker SDK, OpenCV, and GLFW.

## Prerequisites

- **Visual Studio 2022** (or later)
- **CMake 3.21+** (included with Visual Studio)
- **Git** (with submodule support)
- **Teledyne Spinnaker SDK** - Install to `C:/Program Files/Teledyne/Spinnaker`

## Getting Started

### 1. Clone the Repository

Clone the repo with the vcpkg submodule included:

```sh
git clone --recurse-submodules https://github.com/stef1029/behaviour_camera.git
cd behaviour_camera
```

If you've already cloned without `--recurse-submodules`, initialise the submodule manually:

```sh
git submodule update --init
```

### 2. Bootstrap vcpkg

Build the vcpkg package manager from the included submodule:

```sh
.\vcpkg\bootstrap-vcpkg.bat
```

### 3. Configure

Generate the build files using CMake presets. This will also install dependencies (OpenCV, GLFW, nlohmann_json) via vcpkg automatically:

```sh
cmake --preset windows-release
```

This generates Visual Studio solution files in `out/build/vs-release/`.

### 4. Build

Compile the project:

```sh
cmake --build out/build/vs-release --config Release
```

Or open `out/build/vs-release/behaviour_camera.sln` in Visual Studio and press F7.

The executable will be created at `out/build/vs-release/Release/behaviour_camera.exe`.

### 5. Install (Optional — For Deployment)

Create a standalone, portable package with all required DLLs:

```sh
cmake --install out/build/vs-release --config Release
```

This creates a complete deployment folder at `out/install/vs-release/bin/` containing:
- `behaviour_camera.exe`
- All OpenCV DLLs
- All GLFW DLLs
- All Spinnaker SDK DLLs

You can zip the `bin/` folder and deploy it to other machines.

### Quick Start (All Steps)

```sh
git clone --recurse-submodules https://github.com/stef1029/behaviour_camera.git
cd behaviour_camera
.\vcpkg\bootstrap-vcpkg.bat
cmake --preset windows-release
cmake --build out/build/vs-release --config Release
```

## Running the Application

### From Build Directory (Development)

```sh
out\build\vs-release\Release\behaviour_camera.exe --serial_number 22181614 --fps 60
```

### From Install Directory (Deployment)

```sh
out\install\vs-release\bin\behaviour_camera.exe --serial_number 22181614 --fps 60
```

## Command-Line Arguments

| Argument | Description | Default |
|----------|-------------|---------|
| `--id` | Mouse/subject ID | `NoID` |
| `--date` | Date/time string | Current date/time |
| `--path` | Output directory | `E:\test_vid_output\{date}_{id}` |
| `--serial_number` | Camera serial number | Required |
| `--fps` | Frame rate | `60.0` |
| `--windowWidth` | Display window width | `800` |
| `--windowHeight` | Display window height | `600` |

## Project Structure

```
behaviour_camera/
├── CMakeLists.txt           # Build configuration
├── CMakePresets.json        # CMake presets for easy configuration
├── vcpkg.json               # vcpkg dependency manifest
├── .gitmodules              # vcpkg submodule reference
├── src/
│   └── main.cpp             # Main application code
├── vcpkg/                   # vcpkg (git submodule, pinned version)
└── out/
    ├── build/               # Build artifacts
    └── install/             # Deployment package
```

## Troubleshooting

### "SPINNAKER_ROOT not set" Error

The Spinnaker SDK path is configured in `CMakePresets.json`. If installed elsewhere, update:

```json
"SPINNAKER_ROOT": "C:/Path/To/Your/Spinnaker"
```

### Generator Mismatch Error

If you see "generator does not match", delete the build directory and reconfigure:

```sh
rmdir /s /q out\build\vs-release
cmake --preset windows-release
```

### Missing DLLs When Running

If the executable can't find DLLs, run the install step:

```sh
cmake --install out/build/vs-release --config Release
```

Then run from `out/install/vs-release/bin/`.
