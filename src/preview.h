// The preview window: live image plus the numbers that say whether the session is
// going well.
//
// Scaling happens on the GPU now. The old version resized every displayed frame on
// the CPU with cv::resize, on the capture thread, which was both the wrong place to
// spend time and the only thing OpenCV was used for - so OpenCV is gone.
//
// The window also runs on the main thread rather than inside the capture loop, so
// it stays responsive when the camera stalls. That is exactly when you want to look
// at it, and exactly when the old version froze.

#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

struct GLFWwindow;

namespace behaviour_camera {

struct Counters;
class FramePool;
class FrameQueue;

// What the preview shows besides the image itself. Filled in by the recorder each
// time round the display loop.
struct PreviewStatus
{
    std::string rig;
    std::string mouseID;
    std::string sinkDescription;
    std::string pixelFormat;

    int imageWidth = 0;
    int imageHeight = 0;
    double targetFps = 0.0;
    double measuredFps = 0.0;
    double elapsedSeconds = 0.0;

    double writeMegabytesPerSecond = 0.0;
    double diskFreeGigabytes = 0.0;
    double diskSecondsRemaining = -1.0;   // negative if not known

    double exposureMicroseconds = -1.0;   // negative if the camera will not say
    double temperatureCelsius = -1000.0;

    size_t bufferDepth = 0;               // frames waiting to be written
    size_t bufferCapacity = 0;
    size_t bufferHighWater = 0;

    int64_t framesWritten = 0;
    int64_t droppedInTransit = 0;         // the camera made them, we never got them
    int64_t droppedNoBuffer = 0;          // we got them and could not keep up
    int64_t incompleteFrames = 0;
    int64_t cameraResets = 0;
};

// Optional guide drawn over the image, to put the camera back where it was last
// time. Normalised to the image so it is resolution independent.
struct ArenaGuide
{
    bool enabled = false;
    double centreX = 0.5;
    double centreY = 0.5;
    double radius = 0.4;
};

class Preview
{
public:
    // Throws std::runtime_error if the window or GL context cannot be created.
    Preview(int width, int height, const std::string& title,
            int imageWidth, int imageHeight, const ArenaGuide& guide);
    ~Preview();

    Preview(const Preview&) = delete;
    Preview& operator=(const Preview&) = delete;

    // Draws one frame of UI. Pass an empty span to redraw with the previous image,
    // which is what keeps the window alive while the camera is stalled.
    void render(const std::vector<uint8_t>& image, const PreviewStatus& status);

    // True once the user has asked to stop, by Esc or by closing the window. A
    // close is confirmed first: a stray click should not end a two-hour session.
    bool stopRequested() const { return stopRequested_; }

    // Whether the image is being drawn at all. Turning it off leaves the stats
    // visible and costs the capture path nothing.
    bool showingImage() const { return showImage_; }

private:
    void uploadImage(const std::vector<uint8_t>& image);
    void drawImagePanel();
    void drawStatsPanel(const PreviewStatus& status);
    void drawHistogram();
    void computeHistogram(const std::vector<uint8_t>& image);

    GLFWwindow* window_ = nullptr;
    unsigned int texture_ = 0;
    int imageWidth_ = 0;
    int imageHeight_ = 0;
    bool haveImage_ = false;

    ArenaGuide guide_;
    bool showImage_ = true;
    bool showHistogram_ = false;
    bool stopRequested_ = false;
    bool confirmingClose_ = false;

    std::vector<float> histogram_;
    double saturatedFraction_ = 0.0;
};

} // namespace behaviour_camera
