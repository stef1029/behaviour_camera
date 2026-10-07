#include "preview.h"

#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>

#include <GLFW/glfw3.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <stdexcept>

namespace behaviour_camera {
namespace {

constexpr int kHistogramBins = 64;

// Formats a duration as something readable at a glance, which is what the
// time-remaining figure is for.
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

ImVec4 colourForFraction(double fraction)
{
    // Green while there is plenty of slack, amber past half, red when nearly out.
    if (fraction < 0.5) return ImVec4(0.35f, 0.78f, 0.45f, 1.0f);
    if (fraction < 0.8) return ImVec4(0.95f, 0.72f, 0.30f, 1.0f);
    return ImVec4(0.90f, 0.35f, 0.35f, 1.0f);
}

void labelledValue(const char* label, const std::string& value,
                   const ImVec4* colour = nullptr)
{
    ImGui::TextUnformatted(label);
    ImGui::SameLine(190.0f);
    if (colour) {
        ImGui::TextColored(*colour, "%s", value.c_str());
    } else {
        ImGui::TextUnformatted(value.c_str());
    }
}

std::string formatted(const char* fmt, double value)
{
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), fmt, value);
    return buffer;
}

} // namespace

Preview::Preview(int width, int height, const std::string& title,
                 int imageWidth, int imageHeight, const ArenaGuide& guide)
    : imageWidth_(imageWidth), imageHeight_(imageHeight), guide_(guide)
{
    if (!glfwInit()) {
        throw std::runtime_error("Failed to initialise GLFW");
    }

    // ImGui's OpenGL3 backend wants a 3.x context. The old preview used
    // glDrawPixels, which is legacy fixed-function and not available here.
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_COMPAT_PROFILE);

    window_ = glfwCreateWindow(width, height, title.c_str(), nullptr, nullptr);
    if (!window_) {
        glfwTerminate();
        throw std::runtime_error("Failed to create the preview window");
    }

    glfwMakeContextCurrent(window_);
    // No vsync: the display loop paces itself, and waiting on the compositor here
    // would make the window's refresh rate dictate how often stats update.
    glfwSwapInterval(0);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;   // do not litter the working directory
    ImGui::StyleColorsDark();

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

    // Uploaded as a single-channel texture and expanded to grey in the shader by
    // ImGui's sampler, rather than converting on the CPU. The old code assumed
    // 8-bit mono throughout and would have drawn nonsense for any other format;
    // this at least draws the luminance plane of whatever arrives.
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

    // Every 7th pixel: enough for a usable shape, and keeps this well under a
    // millisecond on a 1.3 megapixel frame. 7 rather than 8 so the stride does
    // not align with the row width and sample the same columns every row.
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

void Preview::drawImagePanel()
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

    if (guide_.enabled) {
        // Drawn in image-normalised coordinates so it lands in the same place
        // whatever the window size, which is the point of having it.
        ImDrawList* draw = ImGui::GetWindowDrawList();
        const ImVec2 centre(topLeft.x + static_cast<float>(guide_.centreX) * size.x,
                            topLeft.y + static_cast<float>(guide_.centreY) * size.y);
        const float radius = static_cast<float>(guide_.radius) * size.x;
        draw->AddCircle(centre, radius, IM_COL32(120, 200, 255, 160), 64, 2.0f);
        draw->AddLine(ImVec2(centre.x - 8, centre.y), ImVec2(centre.x + 8, centre.y),
                      IM_COL32(120, 200, 255, 160), 1.5f);
        draw->AddLine(ImVec2(centre.x, centre.y - 8), ImVec2(centre.x, centre.y + 8),
                      IM_COL32(120, 200, 255, 160), 1.5f);
    }
}

void Preview::drawHistogram()
{
    ImGui::PlotHistogram("##histogram", histogram_.data(), kHistogramBins, 0,
                         nullptr, 0.0f, 1.0f, ImVec2(-1.0f, 70.0f));

    const double percent = saturatedFraction_ * 100.0;
    if (percent > 1.0) {
        ImGui::TextColored(ImVec4(0.90f, 0.35f, 0.35f, 1.0f),
                           "%.1f%% of pixels saturated - reduce exposure or light", percent);
    } else {
        ImGui::TextDisabled("%.2f%% saturated", percent);
    }
}

void Preview::drawStatsPanel(const PreviewStatus& status)
{
    ImGui::SeparatorText("Session");
    labelledValue("rig", status.rig);
    labelledValue("subject", status.mouseID);
    labelledValue("elapsed", humanDuration(status.elapsedSeconds));
    labelledValue("writing", status.sinkDescription);

    ImGui::SeparatorText("Capture");
    labelledValue("image", std::to_string(status.imageWidth) + " x " +
                               std::to_string(status.imageHeight) + " " + status.pixelFormat);

    // Below target with no dropped frames usually means exposure is limiting the
    // rate rather than anything going wrong, so this is amber, not red.
    const bool behind = status.targetFps > 0 && status.measuredFps < status.targetFps * 0.95;
    const ImVec4 amber(0.95f, 0.72f, 0.30f, 1.0f);
    labelledValue("rate",
                  formatted("%.1f", status.measuredFps) + " / " +
                      formatted("%.1f fps", status.targetFps),
                  behind ? &amber : nullptr);

    const ImVec4 red2(0.90f, 0.35f, 0.35f, 1.0f);
    labelledValue("frames recorded", std::to_string(status.framesWritten));
    labelledValue("dropped in transit", std::to_string(status.droppedInTransit),
                  status.droppedInTransit > 0 ? &red2 : nullptr);
    labelledValue("dropped, writer behind", std::to_string(status.droppedNoBuffer),
                  status.droppedNoBuffer > 0 ? &red2 : nullptr);
    if (status.incompleteFrames > 0) {
        labelledValue("incomplete", std::to_string(status.incompleteFrames));
    }
    if (status.cameraResets > 0) {
        labelledValue("camera resets", std::to_string(status.cameraResets), &red2);
    }

    ImGui::SeparatorText("Buffer");
    const double fraction = status.bufferCapacity > 0
        ? static_cast<double>(status.bufferDepth) / static_cast<double>(status.bufferCapacity)
        : 0.0;
    const ImVec4 barColour = colourForFraction(fraction);
    ImGui::PushStyleColor(ImGuiCol_PlotHistogram, barColour);
    char overlay[64];
    std::snprintf(overlay, sizeof(overlay), "%zu / %zu", status.bufferDepth, status.bufferCapacity);
    ImGui::ProgressBar(static_cast<float>(fraction), ImVec2(-1.0f, 0.0f), overlay);
    ImGui::PopStyleColor();
    ImGui::TextDisabled("peak %zu - the best early warning that writing is falling behind",
                        status.bufferHighWater);

    ImGui::SeparatorText("Disk");
    labelledValue("write rate", formatted("%.1f MB/s", status.writeMegabytesPerSecond));
    labelledValue("free", formatted("%.1f GB", status.diskFreeGigabytes));
    const bool lowDisk = status.diskSecondsRemaining >= 0 && status.diskSecondsRemaining < 1800;
    const ImVec4 red(0.90f, 0.35f, 0.35f, 1.0f);
    labelledValue("time remaining", humanDuration(status.diskSecondsRemaining),
                  lowDisk ? &red : nullptr);

    ImGui::SeparatorText("Camera");
    if (status.exposureMicroseconds >= 0) {
        labelledValue("exposure", formatted("%.0f us", status.exposureMicroseconds));
    }
    if (status.temperatureCelsius > -100) {
        const bool hot = status.temperatureCelsius > 75.0;
        labelledValue("temperature", formatted("%.1f C", status.temperatureCelsius),
                      hot ? &red : nullptr);
    }
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
    if (guide_.enabled) {
        ImGui::Checkbox("arena guide", &guide_.enabled);
        ImGui::SameLine();
    }
    ImGui::TextDisabled("  Esc to stop");

    const float statsWidth = 320.0f;
    if (ImGui::BeginTable("##layout", 2, ImGuiTableFlags_Resizable)) {
        ImGui::TableSetupColumn("image", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("stats", ImGuiTableColumnFlags_WidthFixed, statsWidth);

        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        if (showImage_) {
            ImGui::BeginChild("##image", ImVec2(0, 0), ImGuiChildFlags_None);
            drawImagePanel();
            ImGui::EndChild();
        } else {
            ImGui::TextDisabled("Image display off. The recording is unaffected.");
        }

        ImGui::TableSetColumnIndex(1);
        ImGui::BeginChild("##stats", ImVec2(0, 0), ImGuiChildFlags_None);
        drawStatsPanel(status);
        if (showHistogram_) {
            ImGui::SeparatorText("Histogram");
            drawHistogram();
        }
        ImGui::EndChild();

        ImGui::EndTable();
    }

    ImGui::End();

    // A click on the window's close button starts a confirmation rather than
    // ending the session. Losing two hours of recording to a stray click is a
    // failure mode worth one extra dialog.
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
