// Where recorded frames go.
//
// Two destinations, chosen by configuration:
//
//   raw    every frame written straight to a .bin, exactly as this program has
//          always done. Largest files, no compression artifacts, no dependencies.
//
//   video  frames piped to ffmpeg and encoded on the GPU. Roughly 20x smaller
//          all-intra, or 100x at GOP 30, and the disk write rate drops with it -
//          which matters because disk stalls are what cost frames.
//
// Both keep the same invariant, and everything downstream depends on it:
//
//     output frame i is the i-th entry of frame_IDs
//
// That is what lets a frame be mapped back to its DAQ pulse. A sink that cannot
// guarantee it must fail loudly rather than produce a file that looks fine.

#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace behaviour_camera {

struct VideoSettings
{
    std::string ffmpeg = "ffmpeg";       // name on PATH, or a full path
    std::string codec = "hevc_nvenc";
    // p5 tops out at ~104 fps per stream with four 1280x1024 cameras on an RTX
    // A4500, short of 120; p2 manages ~176 at the same constant QP, and the
    // files come out the same size. Measured 2026-10-07.
    std::string preset = "p2";

    // Low-latency tune. Needed, not cosmetic: presets p2-p7 enable B-frames, and
    // NVENC refuses a GOP shorter than its B-frame count, so without this an
    // all-intra encode is rejected outright at any preset above p1.
    std::string tune = "ll";

    int qp = 23;                          // lower is better quality, larger files

    // Frames between keyframes. 1 makes every frame independently decodable, which
    // is what MJPG gave. 30 is a keyframe a second: several times smaller, and
    // seeking to an arbitrary frame costs decoding up to 29 extra frames.
    int gop = 30;

    std::string container = "mkv";        // mkv survives an unclean exit; mp4 does not
    std::vector<std::string> extra_args;  // escape hatch for anything not covered
};

class FrameSink
{
public:
    virtual ~FrameSink() = default;

    // False means the frame did not make it and recording should stop. Carrying on
    // after a failed write would break the frame-index invariant silently.
    virtual bool write(const void* data, size_t bytes) = 0;

    // Closes the destination and waits for it to finish. False if it did not end
    // cleanly - for the encoder that includes ffmpeg returning an error.
    virtual bool finish() = 0;

    // How many frames the finished output actually contains, or -1 if it cannot be
    // determined. Checked against the number of frame IDs recorded.
    virtual int64_t frameCount() const = 0;

    virtual std::filesystem::path path() const = 0;
    virtual std::string describe() const = 0;
};

// Writes frames back to back with no header, as before.
std::unique_ptr<FrameSink> makeRawSink(const std::filesystem::path& outputPath);

// Spawns ffmpeg and pipes frames to it. Throws std::runtime_error if ffmpeg cannot
// be started, which is better found now than two hours into a session.
std::unique_ptr<FrameSink> makeVideoSink(const std::filesystem::path& outputStem,
                                         const VideoSettings& settings,
                                         int width, int height, double fps);

} // namespace behaviour_camera
