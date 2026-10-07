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
constexpr float kLabelColumn = 150.0f;

const ImVec4 kGreen(0.40f, 0.80f, 0.50f, 1.0f);
const ImVec4 kAmber(0.95f, 0.72f, 0.30f, 1.0f);
const ImVec4 kRed(0.93f, 0.40f, 0.40f, 1.0f);
const ImVec4 kDim(0.55f, 0.57f, 0.62f, 1.0f);

std::string text(const char* fmt, double value)
{
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), fmt, value);
    return buffer;
}

std::string humanDuration(double seconds)
{
    if (seconds < 0) {
        return "unknown";
    }
    char buffer[64];
    if (seconds < 90) {
        std::snprintf(buffer, sizeof(buffer), "%.0f s", seconds);
    } else if (seconds < 5400) {
        std::snprintf(buffer, sizeof(buffer), "%.0f min", seconds / 60.0);
    } else {
        std::snprintf(buffer, sizeof(buffer), "%.1f h", seconds / 3600.0);
    }
    return buffer;
}

// Green while there is plenty of slack, amber past half, red past 80%.
ImVec4 occupancyColour(double fraction)
{
    if (fraction < 0.5) return kGreen;
    if (fraction < 0.8) return kAmber;
    return kRed;
}

// A label on the left and a value on the right, so the numbers line up and can
// be read down the column rather than hunted for.
void row(const char* label, const std::string& value, const ImVec4* colour = nullptr)
{
    ImGui::TextColored(kDim, "%s", label);
    ImGui::SameLine(kLabelColumn);
    if (colour) {
        ImGui::TextColored(*colour, "%s", value.c_str());
    } else {
        ImGui::TextUnformatted(value.c_str());
    }
}

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
    style.WindowPadding = ImVec2(10, 10);
    style.FramePadding = ImVec2(6, 4);
    style.ItemSpacing = ImVec2(8, 6);
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
    // Updated every drawn frame, not just when a sample is taken.
    const double occupancyNow = status.bufferCapacity > 0
        ? static_cast<double>(status.bufferDepth) / static_cast<double>(status.bufferCapacity)
        : 0.0;
    bufferPeakSinceSample_ = std::max(bufferPeakSinceSample_,
                                      static_cast<float>(occupancyNow * 100.0));

    // Twice a second, so the traces cover about a minute without the cost of
    // sampling every drawn frame.
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

// One line you can read from across the room: is this rig fine or not?
void Preview::drawBanner(const PreviewStatus& status)
{
    const int64_t lost = status.droppedInTransit + status.droppedNoBuffer;
    const bool trouble = lost > 0 || status.cameraResets > 0;

    const ImVec4 colour = trouble ? kRed : kGreen;
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(colour.x * 0.18f, colour.y * 0.18f,
                                                   colour.z * 0.18f, 1.0f));
    ImGui::BeginChild("##banner", ImVec2(0, 56), ImGuiChildFlags_None);
    ImGui::SetCursorPos(ImVec2(12, 8));

    ImGui::PushStyleColor(ImGuiCol_Text, colour);
    ImGui::Text("%s", trouble ? "RECORDING - FRAMES LOST" : "RECORDING");
    ImGui::PopStyleColor();

    ImGui::SetCursorPosX(12);
    if (trouble) {
        ImGui::TextColored(kRed, "%lld frames, %lld lost", static_cast<long long>(status.framesWritten),
                           static_cast<long long>(lost));
    } else {
        ImGui::TextColored(kDim, "%lld frames, none lost, %s",
                           static_cast<long long>(status.framesWritten),
                           humanDuration(status.elapsedSeconds).c_str());
    }

    ImGui::EndChild();
    ImGui::PopStyleColor();
}

void Preview::drawImagePanel(const PreviewStatus& status)
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

    const ImVec2 topLeft = ImGui::GetCursorScreenPos();
    ImGui::Image((ImTextureID)(intptr_t)texture_, size);

    // A few numbers burned into the corner of the image, so a screenshot or a
    // glance at just the video tells you the state without the side panel.
    ImDrawList* draw = ImGui::GetWindowDrawList();
    char line[128];
    const int64_t lost = status.droppedInTransit + status.droppedNoBuffer;
    std::snprintf(line, sizeof(line), "%.1f fps   %lld frames   %lld lost",
                  status.measuredFps, static_cast<long long>(status.framesWritten),
                  static_cast<long long>(lost));

    const ImVec2 textSize = ImGui::CalcTextSize(line);
    const ImVec2 pad(8, 5);
    const ImVec2 boxTopLeft(topLeft.x + 10, topLeft.y + 10);
    draw->AddRectFilled(boxTopLeft,
                        ImVec2(boxTopLeft.x + textSize.x + pad.x * 2,
                               boxTopLeft.y + textSize.y + pad.y * 2),
                        IM_COL32(0, 0, 0, 150), 4.0f);
    draw->AddText(ImVec2(boxTopLeft.x + pad.x, boxTopLeft.y + pad.y),
                  lost > 0 ? IM_COL32(237, 102, 102, 255) : IM_COL32(230, 230, 230, 255),
                  line);
}

void Preview::drawHistogram()
{
    ImGui::PlotHistogram("##histogram", histogram_.data(), kHistogramBins, 0,
                         nullptr, 0.0f, 1.0f, ImVec2(-1.0f, 60.0f));
    const double percent = saturatedFraction_ * 100.0;
    if (percent > 1.0) {
        ImGui::TextColored(kRed, "%.1f%% saturated", percent);
    } else {
        ImGui::TextColored(kDim, "%.2f%% saturated", percent);
    }
}

void Preview::drawStatsPanel(const PreviewStatus& status)
{
    // ---- throughput -------------------------------------------------------
    ImGui::SeparatorText("Throughput");

    const bool behind = status.targetFps > 0 && status.measuredFps < status.targetFps * 0.95;
    row("frame rate", text("%.1f", status.measuredFps) + " / " +
                          text("%.1f fps", status.targetFps),
        behind ? &kAmber : nullptr);

    if (!fpsHistory_.empty()) {
        std::vector<float> values(fpsHistory_.begin(), fpsHistory_.end());
        const float top = static_cast<float>(status.targetFps) * 1.1f;
        const float lowest = *std::min_element(values.begin(), values.end());
        char caption[48];
        std::snprintf(caption, sizeof(caption), "last 60 s, low %.1f", lowest);
        ImGui::PlotLines("##fps", values.data(), static_cast<int>(values.size()), 0,
                         caption, 0.0f, top > 0 ? top : 100.0f, ImVec2(-1.0f, 46.0f));
    }

    // Spacing between arriving frames. A long tail here is a stall that did not
    // quite cost a frame - the warning before one that does.
    if (status.intervalMedianMs > 0) {
        row("interval med/p99", text("%.1f", status.intervalMedianMs) + " / " +
                                    text("%.1f ms", status.intervalP99Ms));
        const bool spike = status.intervalWorstMs > status.intervalMedianMs * 3.0;
        row("worst interval", text("%.1f ms", status.intervalWorstMs),
            spike ? &kAmber : nullptr);
    }

    // ---- buffer -----------------------------------------------------------
    ImGui::SeparatorText("Buffer");

    const double fraction = status.bufferCapacity > 0
        ? static_cast<double>(status.bufferDepth) / static_cast<double>(status.bufferCapacity)
        : 0.0;
    char overlay[64];
    std::snprintf(overlay, sizeof(overlay), "%zu / %zu frames",
                  status.bufferDepth, status.bufferCapacity);
    ImGui::PushStyleColor(ImGuiCol_PlotHistogram, occupancyColour(fraction));
    ImGui::ProgressBar(static_cast<float>(fraction), ImVec2(-1.0f, 0.0f), overlay);
    ImGui::PopStyleColor();

    if (!bufferHistory_.empty()) {
        std::vector<float> values(bufferHistory_.begin(), bufferHistory_.end());
        ImGui::PlotLines("##buffer", values.data(), static_cast<int>(values.size()), 0,
                         "peak occupancy %, last 60 s", 0.0f, 100.0f, ImVec2(-1.0f, 46.0f));
    }

    const double peakFraction = status.bufferCapacity > 0
        ? static_cast<double>(status.bufferHighWater) / static_cast<double>(status.bufferCapacity)
        : 0.0;
    const ImVec4 peakColour = occupancyColour(peakFraction);
    row("peak", std::to_string(status.bufferHighWater) + " (" +
                    text("%.0f%%", peakFraction * 100.0) + ")", &peakColour);

    // ---- frames -----------------------------------------------------------
    ImGui::SeparatorText("Frames");
    row("recorded", std::to_string(status.framesWritten));
    row("lost in transit", std::to_string(status.droppedInTransit),
        status.droppedInTransit > 0 ? &kRed : nullptr);
    row("writer behind", std::to_string(status.droppedNoBuffer),
        status.droppedNoBuffer > 0 ? &kRed : nullptr);
    if (status.incompleteFrames > 0) {
        row("incomplete", std::to_string(status.incompleteFrames), &kAmber);
    }
    if (status.cameraResets > 0) {
        row("camera resets", std::to_string(status.cameraResets), &kRed);
    }

    // ---- camera -----------------------------------------------------------
    ImGui::SeparatorText("Camera");
    if (status.exposureMicroseconds >= 0) {
        // Exposure caps the frame rate: it cannot exceed one frame period.
        const bool capping = status.targetFps > 0 &&
                             status.exposureMicroseconds > 1e6 / status.targetFps * 0.95;
        row("exposure", text("%.0f us", status.exposureMicroseconds),
            capping ? &kAmber : nullptr);
    }
    if (status.temperatureCelsius > -100) {
        const bool hot = status.temperatureCelsius > 75.0;
        row("temperature", text("%.1f C", status.temperatureCelsius), hot ? &kRed : nullptr);
    }

    if (showHistogram_) {
        drawHistogram();
    }

    // ---- disk -------------------------------------------------------------
    ImGui::SeparatorText("Disk");
    row("write rate", text("%.1f MB/s", status.writeMegabytesPerSecond));
    row("free", text("%.0f GB", status.diskFreeGigabytes));
    const bool lowDisk = status.diskSecondsRemaining >= 0 && status.diskSecondsRemaining < 1800;
    row("time remaining", humanDuration(status.diskSecondsRemaining),
        lowDisk ? &kRed : nullptr);

    // ---- session ----------------------------------------------------------
    ImGui::SeparatorText("Session");
    row("rig", status.rig);
    row("subject", status.mouseID);
    row("image", std::to_string(status.imageWidth) + " x " +
                     std::to_string(status.imageHeight) + " " + status.pixelFormat);
    // Wrapped rather than clipped: the encoder description is long and used to
    // run off the edge of the panel.
    ImGui::TextColored(kDim, "output");
    ImGui::SameLine(kLabelColumn);
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextUnformatted(status.sinkDescription.c_str());
    ImGui::PopTextWrapPos();
}

void Preview::render(const std::vector<uint8_t>& image, const PreviewStatus& status)
{
    glfwPollEvents();

    if (!image.empty()) {
        if (showImage_) {
            uploadImage(image);
        }
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

    ImGui::Checkbox("image", &showImage_);
    ImGui::SameLine();
    ImGui::Checkbox("histogram", &showHistogram_);
    ImGui::SameLine();
    ImGui::TextColored(kDim, "   Esc to stop");

    // Wide enough that the longest value does not run off the edge, which the
    // previous 320 px column did for the encoder description and the peak note.
    const float statsWidth = 400.0f;
    if (ImGui::BeginTable("##layout", 2, ImGuiTableFlags_Resizable)) {
        ImGui::TableSetupColumn("image", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("stats", ImGuiTableColumnFlags_WidthFixed, statsWidth);

        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        if (showImage_) {
            ImGui::BeginChild("##image", ImVec2(0, 0), ImGuiChildFlags_None);
            drawImagePanel(status);
            ImGui::EndChild();
        } else {
            ImGui::TextColored(kDim, "Image display off. The recording is unaffected.");
        }

        ImGui::TableSetColumnIndex(1);
        ImGui::BeginChild("##stats", ImVec2(0, 0), ImGuiChildFlags_None);
        drawBanner(status);
        drawStatsPanel(status);
        ImGui::EndChild();

        ImGui::EndTable();
    }

    ImGui::End();

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
        if (ImGui::Button("Stop", ImVec2(120, 0))) {
            stopRequested_ = true;
            confirmingClose_ = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Keep recording", ImVec2(140, 0)) ||
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
