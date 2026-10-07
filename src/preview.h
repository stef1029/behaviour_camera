// The preview window: live image, with the health of the recording on two thin
// banners above and below it.
//
// The rigs open this at 640x512, which is the size it has to work at. A side
// panel leaves almost nothing for the image at that width, so the layout is two
// single-line banners instead and the picture gets everything in between.
//
// The top banner answers one question - is this recording healthy - in a form
// readable from across the room. The bottom carries the numbers behind it, and
// drops the least important ones rather than overflowing when the window is
// narrow. The detail that will not fit on a line, the traces and the interval
// statistics, appears over the image on a keypress.
//
// Scaling happens on the GPU. The old version resized every displayed frame on
// the CPU, on the capture thread, which was both the wrong place to spend time
// and the only thing OpenCV was used for. The window also runs on the main
// thread rather than inside the capture loop, so it stays responsive when the
// camera stalls - which is exactly when you want to look at it.

#pragma once

#include <cstdint>
#include <deque>
#include <string>
#include <vector>

struct GLFWwindow;

namespace behaviour_camera {

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

    // Spacing between arriving frames, in milliseconds. The tail matters more
    // than the middle: a long p99 is a stall that nearly cost a frame.
    double intervalMedianMs = 0.0;
    double intervalP99Ms = 0.0;
    double intervalWorstMs = 0.0;

    size_t bufferDepth = 0;               // frames waiting to be written
    size_t bufferCapacity = 0;
    size_t bufferHighWater = 0;

    int64_t framesWritten = 0;
    int64_t droppedInTransit = 0;         // the camera made them, we never got them
    int64_t droppedNoBuffer = 0;          // we got them and could not keep up
    int64_t incompleteFrames = 0;
    int64_t cameraResets = 0;
};

class Preview
{
public:
    // Throws std::runtime_error if the window or GL context cannot be created.
    Preview(int width, int height, const std::string& title,
            int imageWidth, int imageHeight, bool showHistogram);
    ~Preview();

    Preview(const Preview&) = delete;
    Preview& operator=(const Preview&) = delete;

    // Draws one frame of UI. An empty image redraws the previous one, which is
    // what keeps the window alive while the camera is stalled.
    void render(const std::vector<uint8_t>& image, const PreviewStatus& status);

    // True once the user has asked to stop, by Esc or by closing the window. A
    // close is confirmed first: a stray click should not end a two-hour session.
    bool stopRequested() const { return stopRequested_; }

private:
    void uploadImage(const std::vector<uint8_t>& image);
    void sampleHistory(const PreviewStatus& status);
    void drawTopBanner(const PreviewStatus& status, float height);
    void drawBottomBanner(const PreviewStatus& status, float height);
    void drawImage();
    void drawDetailOverlay(const PreviewStatus& status);
    void drawHistogramOverlay();
    void computeHistogram(const std::vector<uint8_t>& image);

    GLFWwindow* window_ = nullptr;
    unsigned int texture_ = 0;
    int imageWidth_ = 0;
    int imageHeight_ = 0;
    bool haveImage_ = false;

    bool showDetails_ = false;
    bool showHistogram_ = false;
    bool stopRequested_ = false;
    bool confirmingClose_ = false;

    // Roughly the last minute, sampled twice a second.
    std::deque<float> fpsHistory_;
    std::deque<float> bufferHistory_;
    double lastSampleSeconds_ = -1.0;
    // Highest occupancy since the last plotted sample. The buffer fills and
    // drains far faster than the plot ticks, so plotting the instantaneous value
    // misses every spike - which is the only thing the trace is there to show.
    float bufferPeakSinceSample_ = 0.0f;

    std::vector<float> histogram_;
    double saturatedFraction_ = 0.0;
};

} // namespace behaviour_camera
