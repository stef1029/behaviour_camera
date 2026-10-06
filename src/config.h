// Per-camera settings, loaded from a JSON file rather than compiled in.
//
// Adding a camera or changing a rig's frame rate used to mean editing main.cpp and
// redeploying to every rig machine. Settings now come from a file, resolved in
// layers: built-in defaults, then the file's "defaults" block, then the per-camera
// block, then anything given on the command line. Each layer only overrides what it
// actually specifies.
//
// A camera that is not in the file is not an error. It records exactly the same; it
// just gets a rig name derived from its serial.

#pragma once

#include <nlohmann/json.hpp>

#include <optional>
#include <string>

namespace behaviour_camera {

// Everything the recorder needs to know for one session. Values here are already
// fully resolved - no further defaulting happens after this is built.
struct Settings
{
    std::string rig;                    // names the window and the signal files
    std::string output_root;            // session folders are created inside this

    double fps = 60.0;                  // requested; the camera clamps it in practice
    double exposure_lower_limit_us = 4000.0;
    int strobe_line = 2;                // GPIO line driven as an output per frame

    // How many frames the driver may hold while the writer is busy. The camera
    // default of 10 is only a third of a second at 30 fps, so a brief disk stall
    // costs frames; a few hundred buys seconds of slack for a trivial amount of RAM.
    int stream_buffers = 300;

    int window_width = 800;
    int window_height = 600;
    int display_fps = 30;               // preview only; never affects what is recorded

    // Written into the session metadata so each recording carries the settings it
    // actually ran with, rather than leaving them to be inferred later.
    nlohmann::json toJson() const;
};

// Where a config file was found, and what it said. Kept separate from Settings so
// the recorder can report which file it used.
struct ConfigFile
{
    std::string path;                   // empty if no file was found
    nlohmann::json contents;            // null if no file was found

    bool found() const { return !path.empty(); }
};

// Looks for the config file, in order:
//   1. explicitPath, if given (an error if it does not exist)
//   2. the BEHAVIOUR_CAMERA_CONFIG environment variable
//   3. behaviour_camera.json next to the executable
//   4. config/behaviour_camera.json next to the executable, and one level up
//   5. the current working directory
// Not finding one is not an error: built-in defaults are used.
// Throws std::runtime_error if a file is found but cannot be parsed.
ConfigFile findConfig(const std::string& explicitPath);

// Applies the defaults block and then the per-camera block for this serial.
// Unknown serials fall back to a rig name of "cam_<serial>".
Settings resolveSettings(const ConfigFile& config, const std::string& serial);

} // namespace behaviour_camera
