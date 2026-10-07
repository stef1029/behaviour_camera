// Camera settings: sensible defaults in code, overridden per run.
//
// Every value here has a default that works, so the recorder needs no
// configuration file and no setup to run. Anything that genuinely differs
// between rigs is passed on the command line instead, and the launcher fills
// those in from each rig's `camera:` section in rigs.yaml.
//
// That makes rigs.yaml the single place rig hardware is described. There is
// deliberately no config file beside the executable: two files describing the
// same cameras is two files that can disagree.

#pragma once

#include <nlohmann/json.hpp>

#include "frame_sink.h"

#include <string>

namespace behaviour_camera {

struct Settings
{
    // Names the preview window and the signal files the launcher watches for.
    // Left empty, it is derived from the camera's serial number.
    std::string rig;

    // Only used when --path is not given, which it always is from the launcher.
    std::string output_root = "E:/test_vid_output";

    double fps = 60.0;                        // clamped to what the camera allows

    // Auto-exposure will not go below this. Raising it brightens a dark arena at
    // the cost of motion blur, and caps the achievable frame rate: 4000 us limits
    // the camera to 250 fps. Varies with each rig's illumination.
    double exposure_lower_limit_us = 4000.0;

    // GPIO line driven as an output once per captured frame, which is what the
    // DAQ timestamps to give every frame a time.
    int strobe_line = 2;

    // Frames the driver may hold while the writer is busy. The camera's own
    // default is 10 - a third of a second at 30 fps, short enough that an
    // ordinary disk stall drops frames. 300 is about ten seconds of slack and
    // costs roughly 375 MB of RAM.
    int stream_buffers = 300;

    // "raw" writes every frame to a .bin, as the rigs have always done. "video"
    // encodes on the GPU while recording: far smaller, and far less written to
    // disk, so there is no conversion step afterwards.
    std::string recording_mode = "raw";
    VideoSettings video;

    // RAM held between capture and writing. This is the slack that absorbs a
    // disk stall: at 1.25 MiB a frame, a gigabyte is about 800 frames, roughly
    // half a minute at 30 fps. It is allocated up front, so four cameras on one
    // machine cost four times this.
    int ring_buffer_mb = 1024;

    // Publish each frame to a shared memory block so another process can read
    // it for live pose estimation. Off by default: it costs about 0.3 ms of the
    // capture thread per frame, which is worth nothing to a rig not using it.
    bool frame_out = false;

    // Start with the exposure histogram visible, for setting a camera up.
    bool show_histogram = false;

    int window_width = 800;
    int window_height = 600;
    int display_fps = 30;                     // preview only; never what is recorded

    // Written into the session metadata, so each recording carries the settings it
    // actually ran with rather than leaving them to be inferred later.
    nlohmann::json toJson() const;
};

} // namespace behaviour_camera
