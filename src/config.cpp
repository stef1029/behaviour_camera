#include "config.h"

namespace behaviour_camera {

nlohmann::json Settings::toJson() const
{
    nlohmann::json out = {
        { "rig", rig },
        { "fps", fps },
        { "exposure_lower_limit_us", exposure_lower_limit_us },
        { "strobe_line", strobe_line },
        { "stream_buffers", stream_buffers },
        { "window_width", window_width },
        { "window_height", window_height },
        { "display_fps", display_fps },
        { "recording_mode", recording_mode },
        { "ring_buffer_mb", ring_buffer_mb },
    };

    // Only meaningful for an encoded session, and including it for a raw one
    // would suggest settings that had no effect on the file.
    if (recording_mode == "video") {
        out["video"] = {
            { "codec", video.codec },
            { "preset", video.preset },
            { "tune", video.tune },
            { "qp", video.qp },
            { "gop", video.gop },
            { "container", video.container },
        };
    }

    return out;
}

} // namespace behaviour_camera
