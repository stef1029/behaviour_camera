# Behaviour Camera

High-speed camera capture application using Teledyne Spinnaker SDK, OpenCV, and GLFW.

## Prerequisites

- **Visual Studio 2026** (or 2022)
- **CMake 3.21+** (included with Visual Studio)
- **vcpkg** (included in this repository)
- **Teledyne Spinnaker SDK** - Install to `C:/Program Files/Teledyne/Spinnaker`

## Building the Project

### 1. Configure

Generate the build files using CMake presets:

```sh
cmake --preset windows-release
```

This will:
- Install dependencies via vcpkg (OpenCV, GLFW, nlohmann_json)
- Find the Spinnaker SDK
- Generate Visual Studio solution files in `out/build/vs-release/`

### 2. Build

Compile the project:

```sh
cmake --build out/build/vs-release --config Release
```

Or open `out/build/vs-release/behaviour_camera.sln` in Visual Studio and press F7.

The executable will be created at: `out/build/vs-release/Release/behaviour_camera.exe`

### 3. Install (Optional - For Deployment)

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
├── src/
│   └── main.cpp             # Main application code
├── vcpkg/                   # Local vcpkg installation
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
