#include "preview.h"

#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>

#include <GLFW/glfw3.h>

#include <algorithm>
#include <cstdio>
#include <stdexcept>

namespace behaviour_camera {
namespace {

constexpr int kHistogramBins = 64;
constexpr size_t kHistoryLength = 120;      // ~60 s at two samples a second

const ImVec4 kGreen(0.45f, 0.85f, 0.55f, 1.0f);
const ImVec4 kAmber(0.97f, 0.76f, 0.35f, 1.0f);
const ImVec4 kRed(0.97f, 0.45f, 0.45f, 1.0f);
const ImVec4 kDim(0.60f, 0.63f, 0.68f, 1.0f);

std::string text(const char* fmt, double value)
{
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), fmt, value);
    return buffer;
}

std::string humanDuration(double seconds)
{
    if (seconds < 0) {
        return "?";
    }
    char buffer[48];
    if (seconds < 90) {
        std::snprintf(buffer, sizeof(buffer), "%.0fs", seconds);
    } else if (seconds < 5400) {
        std::snprintf(buffer, sizeof(buffer), "%.0fmin", seconds / 60.0);
    } else {
        std::snprintf(buffer, sizeof(buffer), "%.1fh", seconds / 3600.0);
    }
    return buffer;
}

std::string elapsedClock(double seconds)
{
    const int total = static_cast<int>(seconds);
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%d:%02d:%02d",
                  total / 3600, (total / 60) % 60, total % 60);
    return buffer;
}

// Green while there is plenty of slack, amber past half, red past 80%.
ImVec4 occupancyColour(double fraction)
{
    if (fraction < 0.5) return kGreen;
    if (fraction < 0.8) return kAmber;
    return kRed;
}

// One piece of the bottom banner, with the colour it should be drawn in and how
// readily it can be dropped when the window is too narrow to hold everything.
struct Segment
{
    std::string label;
    ImVec4 colour;
    int priority;    // lower is kept longer
};

} // namespace

Preview::Preview(int width, int height, const std::string& title,
                 int imageWidth, int imageHeight, bool showHistogram)
    : imageWidth_(imageWidth), imageHeight_(imageHeight),
      showHistogram_(showHistogram)
{
    if (!glfwInit()) {
        throw std::runtime_error("Failed to initialise GLFW");
    }

    // ImGui's OpenGL3 backend wants a 3.x context. Compatibility profile because
    // the image is uploaded as GL_LUMINANCE, which is GL 1.1 and samples as grey
    // without needing a swizzle or a loader for the core-profile equivalents.
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_COMPAT_PROFILE);

    window_ = glfwCreateWindow(width, height, title.c_str(), nullptr, nullptr);
    if (!window_) {
        glfwTerminate();
        throw std::runtime_error("Failed to create the preview window");
    }

    glfwMakeContextCurrent(window_);
    // No vsync: the display loop paces itself, and waiting on the compositor
    // would let the monitor's refresh rate dictate how often the stats update.
    glfwSwapInterval(0);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;   // do not litter the working directory
    ImGui::StyleColorsDark();

    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowPadding = ImVec2(0, 0);
    style.ItemSpacing = ImVec2(6, 4);
    style.WindowBorderSize = 0.0f;

    if (!ImGui_ImplGlfw_InitForOpenGL(window_, true) ||
        !ImGui_ImplOpenGL3_Init("#version 330")) {
        throw std::runtime_error("Failed to initialise the ImGui backends");
    }

    glGenTextures(1, &texture_);
    glBindTexture(GL_TEXTURE_2D, texture_);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP);

    histogram_.assign(kHistogramBins, 0.0f);
}

Preview::~Preview()
{
    if (texture_ != 0) {
        glDeleteTextures(1, &texture_);
    }
    if (window_ != nullptr) {
        ImGui_ImplOpenGL3_Shutdown();
        ImGui_ImplGlfw_Shutdown();
        ImGui::DestroyContext();
        glfwDestroyWindow(window_);
    }
    glfwTerminate();
}

void Preview::uploadImage(const std::vector<uint8_t>& image)
{
    const size_t expected = static_cast<size_t>(imageWidth_) * imageHeight_;
    if (image.size() < expected) {
        return;
    }

    glBindTexture(GL_TEXTURE_2D, texture_);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);

    // The luminance plane of whatever arrives. The old code hardcoded 8-bit mono
    // throughout and would have drawn nonsense for any other format.
    if (!haveImage_) {
        glTexImage2D(GL_TEXTURE_2D, 0, GL_LUMINANCE, imageWidth_, imageHeight_, 0,
                     GL_LUMINANCE, GL_UNSIGNED_BYTE, image.data());
        haveImage_ = true;
    } else {
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, imageWidth_, imageHeight_,
                        GL_LUMINANCE, GL_UNSIGNED_BYTE, image.data());
    }
}

void Preview::computeHistogram(const std::vector<uint8_t>& image)
{
    std::vector<uint32_t> counts(kHistogramBins, 0);
    uint64_t saturated = 0;

    // Every 7th pixel: enough for a usable shape and well under a millisecond on
    // a 1.3 megapixel frame. 7 rather than 8 so the stride does not align with
    // the row width and sample the same columns on every row.
    const size_t stride = 7;
    size_t sampled = 0;
    for (size_t i = 0; i < image.size(); i += stride) {
        const uint8_t value = image[i];
        counts[static_cast<size_t>(value) * kHistogramBins / 256]++;
        if (value >= 254) {
            ++saturated;
        }
        ++sampled;
    }

    const uint32_t peak = *std::max_element(counts.begin(), counts.end());
    for (int bin = 0; bin < kHistogramBins; ++bin) {
        histogram_[bin] = peak > 0 ? static_cast<float>(counts[bin]) / peak : 0.0f;
    }
    saturatedFraction_ = sampled > 0 ? static_cast<double>(saturated) / sampled : 0.0;
}

void Preview::sampleHistory(const PreviewStatus& status)
{
    // Updated every drawn frame, not only when a sample is taken.
    const double occupancyNow = status.bufferCapacity > 0
        ? static_cast<double>(status.bufferDepth) / static_cast<double>(status.bufferCapacity)
        : 0.0;
    bufferPeakSinceSample_ = std::max(bufferPeakSinceSample_,
                                      static_cast<float>(occupancyNow * 100.0));

    if (lastSampleSeconds_ >= 0 && status.elapsedSeconds - lastSampleSeconds_ < 0.5) {
        return;
    }
    // Nothing until the first real rate measurement, otherwise the startup zero
    // sits in the trace forever and makes the window minimum meaningless.
    if (fpsHistory_.empty() && status.measuredFps <= 0.0) {
        return;
    }
    lastSampleSeconds_ = status.elapsedSeconds;

    fpsHistory_.push_back(static_cast<float>(status.measuredFps));
    bufferHistory_.push_back(bufferPeakSinceSample_);
    bufferPeakSinceSample_ = 0.0f;

    while (fpsHistory_.size() > kHistoryLength) fpsHistory_.pop_front();
    while (bufferHistory_.size() > kHistoryLength) bufferHistory_.pop_front();
}

// The one line you can read from across the room.
void Preview::drawTopBanner(const PreviewStatus& status, float height)
{
    const int64_t lost = status.droppedInTransit + status.droppedNoBuffer;
    const bool trouble = lost > 0 || status.cameraResets > 0;
    const ImVec4 colour = trouble ? kRed : kGreen;

    ImGui::PushStyleColor(ImGuiCol_ChildBg,
                          ImVec4(colour.x * 0.16f, colour.y * 0.16f, colour.z * 0.16f, 1.0f));
    ImGui::BeginChild("##top", ImVec2(0, height), ImGuiChildFlags_None,
                      ImGuiWindowFlags_NoScrollbar);

    ImGui::SetCursorPos(ImVec2(10, (height - ImGui::GetTextLineHeight()) * 0.5f));
    ImGui::TextColored(colour, "%s", trouble ? "FRAMES LOST" : "RECORDING");

    ImGui::SameLine();
    ImGui::TextColored(kDim, " %s ", status.rig.c_str());
    ImGui::SameLine();
    ImGui::Text("%lld frames", static_cast<long long>(status.framesWritten));
    ImGui::SameLine();
    if (lost > 0) {
        ImGui::TextColored(kRed, " %lld lost", static_cast<long long>(lost));
    } else {
        ImGui::TextColored(kDim, " none lost");
    }
    ImGui::SameLine();
    ImGui::TextColored(kDim, " %s", elapsedClock(status.elapsedSeconds).c_str());

    ImGui::EndChild();
    ImGui::PopStyleColor();
}

// The numbers behind the banner, trimmed from the least important end if the
// window is narrow rather than running off the edge.
void Preview::drawBottomBanner(const PreviewStatus& status, float height)
{
    std::vector<Segment> segments;

    const bool behind = status.targetFps > 0 && status.measuredFps < status.targetFps * 0.95;
    segments.push_back({ text("%.1f", status.measuredFps) + "/" +
                             text("%.0f fps", status.targetFps),
                         behind ? kAmber : kGreen, 0 });

    const double fraction = status.bufferCapacity > 0
        ? static_cast<double>(status.bufferDepth) / static_cast<double>(status.bufferCapacity)
        : 0.0;
    const double peakFraction = status.bufferCapacity > 0
        ? static_cast<double>(status.bufferHighWater) / static_cast<double>(status.bufferCapacity)
        : 0.0;
    segments.push_back({ "buf " + text("%.0f%%", fraction * 100.0) +
                             " pk " + text("%.0f%%", peakFraction * 100.0),
                         occupancyColour(peakFraction), 1 });

    segments.push_back({ text("%.0f MB/s", status.writeMegabytesPerSecond), kDim, 2 });

    const bool lowDisk = status.diskSecondsRemaining >= 0 && status.diskSecondsRemaining < 1800;
    segments.push_back({ "disk " + humanDuration(status.diskSecondsRemaining),
                         lowDisk ? kRed : kDim, 2 });

    if (status.exposureMicroseconds >= 0) {
        // Exposure caps the frame rate: it cannot exceed one frame period.
        const bool capping = status.targetFps > 0 &&
                             status.exposureMicroseconds > 1e6 / status.targetFps * 0.95;
        segments.push_back({ "exp " + text("%.1f ms", status.exposureMicroseconds / 1000.0),
                             capping ? kAmber : kDim, 3 });
    }

    if (status.intervalMedianMs > 0) {
        segments.push_back({ "p99 " + text("%.1f ms", status.intervalP99Ms), kDim, 4 });
    }

    if (status.temperatureCelsius > -100) {
        const bool hot = status.temperatureCelsius > 75.0;
        segments.push_back({ text("%.0f C", status.temperatureCelsius),
                             hot ? kRed : kDim, 5 });
    }

    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.10f, 0.10f, 0.12f, 1.0f));
    ImGui::BeginChild("##bottom", ImVec2(0, height), ImGuiChildFlags_None,
                      ImGuiWindowFlags_NoScrollbar);

    const char* hint = "d detail   h histogram   esc stop";
    const float hintWidth = ImGui::CalcTextSize(hint).x;
    const float separator = ImGui::CalcTextSize("   ").x;
    const float available = ImGui::GetContentRegionAvail().x - 20.0f;

    // Drop from the least important end until what remains fits, so a narrow
    // window loses the temperature rather than the frame rate. The key hint is
    // not reserved space here: it is a reminder you read once, so the numbers
    // keep the room and the hint appears only if it still fits afterwards.
    int cutPriority = 5;
    float used = 0.0f;
    while (cutPriority > 0) {
        used = 0.0f;
        for (const Segment& segment : segments) {
            if (segment.priority <= cutPriority) {
                used += ImGui::CalcTextSize(segment.label.c_str()).x + separator;
            }
        }
        if (used <= available) {
            break;
        }
        --cutPriority;
    }
    const bool roomForHint = used + hintWidth + separator <= available;

    ImGui::SetCursorPos(ImVec2(10, (height - ImGui::GetTextLineHeight()) * 0.5f));
    bool first = true;
    for (const Segment& segment : segments) {
        if (segment.priority > cutPriority) {
            continue;
        }
        if (!first) {
            ImGui::SameLine(0.0f, separator);
        }
        first = false;
        ImGui::TextColored(segment.colour, "%s", segment.label.c_str());
    }

    // Right-aligned, so the keys sit in the same place whatever else is shown.
    if (roomForHint) {
        const float hintX = ImGui::GetWindowWidth() - hintWidth - 10.0f;
        ImGui::SameLine();
        if (hintX > ImGui::GetCursorPosX()) {
            ImGui::SetCursorPosX(hintX);
        }
        ImGui::TextColored(ImVec4(0.42f, 0.44f, 0.48f, 1.0f), "%s", hint);
    }

    ImGui::EndChild();
    ImGui::PopStyleColor();
}

void Preview::drawImage()
{
    const ImVec2 available = ImGui::GetContentRegionAvail();
    if (available.x <= 1.0f || available.y <= 1.0f || !haveImage_) {
        return;
    }

    // Letterboxed rather than stretched. The old preview scaled to the window
    // regardless of aspect ratio, so anything judged by eye from it was
    // geometrically wrong.
    const float imageAspect = static_cast<float>(imageWidth_) / static_cast<float>(imageHeight_);
    const float panelAspect = available.x / available.y;

    ImVec2 size = available;
    if (panelAspect > imageAspect) {
        size.x = available.y * imageAspect;
    } else {
        size.y = available.x / imageAspect;
    }

    const ImVec2 offset((available.x - size.x) * 0.5f, (available.y - size.y) * 0.5f);
    ImGui::SetCursorPos(ImVec2(ImGui::GetCursorPosX() + offset.x,
                               ImGui::GetCursorPosY() + offset.y));
    ImGui::Image((ImTextureID)(intptr_t)texture_, size);
}

// The things that will not fit on a line: the traces, and the interval tail.
// Drawn over the image rather than beside it, so the window can stay the size
// the behaviour system opens it at.
void Preview::drawDetailOverlay(const PreviewStatus& status)
{
    const ImVec2 parent = ImGui::GetWindowPos();
    ImGui::SetNextWindowBgAlpha(0.90f);
    ImGui::SetNextWindowPos(ImVec2(parent.x + 10, parent.y + 40), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(320, 0), ImGuiCond_Always);

    if (ImGui::Begin("##detail", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_AlwaysAutoResize)) {
        if (!fpsHistory_.empty()) {
            std::vector<float> values(fpsHistory_.begin(), fpsHistory_.end());
            const float lowest = *std::min_element(values.begin(), values.end());
            const float top = static_cast<float>(status.targetFps) * 1.1f;
            char caption[48];
            std::snprintf(caption, sizeof(caption), "fps, 60s, low %.1f", lowest);
            ImGui::PlotLines("##fps", values.data(), static_cast<int>(values.size()), 0,
                             caption, 0.0f, top > 0 ? top : 100.0f, ImVec2(-1.0f, 38.0f));
        }

        if (!bufferHistory_.empty()) {
            std::vector<float> values(bufferHistory_.begin(), bufferHistory_.end());
            ImGui::PlotLines("##buffer", values.data(), static_cast<int>(values.size()), 0,
                             "buffer peak %, 60s", 0.0f, 100.0f, ImVec2(-1.0f, 38.0f));
        }

        if (status.intervalMedianMs > 0) {
            ImGui::TextColored(kDim, "interval med %.1f  p99 %.1f  worst %.1f ms",
                               status.intervalMedianMs, status.intervalP99Ms,
                               status.intervalWorstMs);
        }
        ImGui::TextColored(kDim, "lost %lld in transit, %lld writer behind",
                           static_cast<long long>(status.droppedInTransit),
                           static_cast<long long>(status.droppedNoBuffer));
        if (status.incompleteFrames > 0 || status.cameraResets > 0) {
            ImGui::TextColored(kAmber, "incomplete %lld   resets %lld",
                               static_cast<long long>(status.incompleteFrames),
                               static_cast<long long>(status.cameraResets));
        }
        ImGui::TextColored(kDim, "%s  %d x %d %s", status.mouseID.c_str(),
                           status.imageWidth, status.imageHeight,
                           status.pixelFormat.c_str());
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextColored(kDim, "%s", status.sinkDescription.c_str());
        ImGui::PopTextWrapPos();
    }
    ImGui::End();
}

void Preview::drawHistogramOverlay()
{
    const ImVec2 parent = ImGui::GetWindowPos();
    const ImVec2 size = ImGui::GetWindowSize();
    ImGui::SetNextWindowBgAlpha(0.90f);
    ImGui::SetNextWindowPos(ImVec2(parent.x + size.x - 262, parent.y + 40), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(250, 0), ImGuiCond_Always);

    if (ImGui::Begin("##histogram", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::PlotHistogram("##hist", histogram_.data(), kHistogramBins, 0,
                             "exposure", 0.0f, 1.0f, ImVec2(-1.0f, 46.0f));
        const double percent = saturatedFraction_ * 100.0;
        if (percent > 1.0) {
            ImGui::TextColored(kRed, "%.1f%% saturated", percent);
        } else {
            ImGui::TextColored(kDim, "%.2f%% saturated", percent);
        }
    }
    ImGui::End();
}

void Preview::render(const std::vector<uint8_t>& image, const PreviewStatus& status)
{
    glfwPollEvents();

    if (!image.empty()) {
        uploadImage(image);
        if (showHistogram_) {
            computeHistogram(image);
        }
    }
    sampleHistory(status);

    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();

    int displayWidth = 0;
    int displayHeight = 0;
    glfwGetFramebufferSize(window_, &displayWidth, &displayHeight);

    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(ImVec2(static_cast<float>(displayWidth),
                                    static_cast<float>(displayHeight)));
    ImGui::Begin("##root", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                 ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus);

    // Thin enough that the picture keeps nearly all of a 640x512 window.
    const float bannerHeight = ImGui::GetTextLineHeight() + 10.0f;

    drawTopBanner(status, bannerHeight);

    const float imageHeight =
        ImGui::GetContentRegionAvail().y - bannerHeight - ImGui::GetStyle().ItemSpacing.y;
    ImGui::BeginChild("##image", ImVec2(0, std::max(imageHeight, 1.0f)),
                      ImGuiChildFlags_None, ImGuiWindowFlags_NoScrollbar);
    drawImage();
    ImGui::EndChild();

    drawBottomBanner(status, bannerHeight);

    if (showDetails_) {
        drawDetailOverlay(status);
    }
    if (showHistogram_) {
        drawHistogramOverlay();
    }

    ImGui::End();

    if (ImGui::IsKeyPressed(ImGuiKey_D)) {
        showDetails_ = !showDetails_;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_H)) {
        showHistogram_ = !showHistogram_;
    }

    // A click on the close button starts a confirmation rather than ending the
    // session. Losing two hours to a stray click is worth one extra dialog.
    if (glfwWindowShouldClose(window_)) {
        glfwSetWindowShouldClose(window_, GLFW_FALSE);
        confirmingClose_ = true;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
        confirmingClose_ = true;
    }

    if (confirmingClose_) {
        ImGui::OpenPopup("Stop recording?");
    }
    if (ImGui::BeginPopupModal("Stop recording?", nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextUnformatted("Stop the session and finalise the recording?");
        ImGui::Separator();
        if (ImGui::Button("Stop", ImVec2(110, 0))) {
            stopRequested_ = true;
            confirmingClose_ = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Keep recording", ImVec2(130, 0)) ||
            ImGui::IsKeyPressed(ImGuiKey_Escape)) {
            confirmingClose_ = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    ImGui::Render();
    glViewport(0, 0, displayWidth, displayHeight);
    glClearColor(0.07f, 0.07f, 0.09f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    glfwSwapBuffers(window_);
}

} // namespace behaviour_camera
