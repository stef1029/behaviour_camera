// Standard library includes
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

// Third-party library includes
#include <GLFW/glfw3.h>  // Must be included before any OpenGL headers
#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>
#include <nlohmann/json.hpp>

// Spinnaker SDK includes
#include <Spinnaker.h>
#include <SpinGenApi/SpinnakerGenApi.h>

#include "config.h"
#include "frame_sink.h"

using namespace Spinnaker;
using namespace Spinnaker::GenApi;
using namespace Spinnaker::GenICam;
using namespace std;
using namespace std::chrono;
using json = nlohmann::json;
namespace fs = std::filesystem;
using behaviour_camera::Settings;
using behaviour_camera::FrameSink;

namespace {

// Returns a Spinnaker image to the driver however the scope is left, including by an
// exception. A frame that is not released is never reused, so a handful of leaked
// frames starves the camera - which used to be possible whenever the display path
// threw partway through handling one.
class ScopedImage
{
public:
    explicit ScopedImage(ImagePtr image) : image_(image) {}

    ~ScopedImage()
    {
        try {
            if (image_.IsValid()) {
                image_->Release();
            }
        }
        catch (Spinnaker::Exception&) {
            // Nothing useful to do from a destructor, and throwing here would
            // terminate the process.
        }
    }

    ScopedImage(const ScopedImage&) = delete;
    ScopedImage& operator=(const ScopedImage&) = delete;

    ImagePtr& get() { return image_; }

private:
    ImagePtr image_;
};

// Sends everything written to a stream to a second one as well, so console output is
// also captured in a session log. Rig machines are launched without a console, where
// otherwise every diagnostic message is lost precisely when it is wanted.
class TeeBuffer : public std::streambuf
{
public:
    TeeBuffer(std::streambuf* first, std::streambuf* second)
        : first_(first), second_(second) {}

protected:
    int overflow(int ch) override
    {
        if (ch == traits_type::eof()) {
            return !traits_type::eof();
        }
        const int a = first_->sputc(static_cast<char>(ch));
        const int b = second_->sputc(static_cast<char>(ch));
        return (a == traits_type::eof() || b == traits_type::eof()) ? traits_type::eof() : ch;
    }

    int sync() override
    {
        const int a = first_->pubsync();
        const int b = second_->pubsync();
        return (a == 0 && b == 0) ? 0 : -1;
    }

private:
    std::streambuf* first_;
    std::streambuf* second_;
};

// Redirects cout and cerr into a log file as well as the console, and puts them back
// when it goes out of scope.
class SessionLog
{
public:
    explicit SessionLog(const fs::path& logPath)
    {
        file_.open(logPath, ios::out | ios::app);
        if (!file_.is_open()) {
            cerr << "Warning: could not open the session log " << logPath
                 << "; messages will only go to the console." << endl;
            return;
        }

        outTee_ = make_unique<TeeBuffer>(cout.rdbuf(), file_.rdbuf());
        errTee_ = make_unique<TeeBuffer>(cerr.rdbuf(), file_.rdbuf());
        oldOut_ = cout.rdbuf(outTee_.get());
        oldErr_ = cerr.rdbuf(errTee_.get());
    }

    ~SessionLog()
    {
        if (oldOut_) cout.rdbuf(oldOut_);
        if (oldErr_) cerr.rdbuf(oldErr_);
    }

    SessionLog(const SessionLog&) = delete;
    SessionLog& operator=(const SessionLog&) = delete;

private:
    ofstream file_;
    unique_ptr<TeeBuffer> outTee_;
    unique_ptr<TeeBuffer> errTee_;
    streambuf* oldOut_ = nullptr;
    streambuf* oldErr_ = nullptr;
};

string currentDateTime()
{
    const auto now = system_clock::now();
    const time_t nowTime = system_clock::to_time_t(now);
    char buffer[80];
    tm localTime{};
    localtime_s(&localTime, &nowTime);
    strftime(buffer, sizeof(buffer), "%y%m%d_%H%M%S", &localTime);
    return string(buffer);
}

} // namespace

class CameraRecorder
{
public:
    CameraRecorder(const string& mouse_ID, const string& start_time, const string& path,
                   const string& serial_number, const Settings& settings)
        : mouse_ID(mouse_ID), start_time(start_time), path(path),
          camSerial(serial_number), settings(settings), rig(settings.rig),
          FPS(settings.fps), windowWidth(settings.window_width),
          windowHeight(settings.window_height), frame_count(0)
    {
        spinSystem = System::GetInstance();
        CameraList camList = spinSystem->GetCameras();

        title = "Rig " + rig + ". Press 'Esc' to stop session.";

        pCam = camList.GetBySerial(camSerial);
        camList.Clear();

        if (!pCam) {
            throw runtime_error("No camera with serial " + camSerial +
                                ". Run camera_probe to see which cameras are attached.");
        }

        pCam->Init();

        // Order matters here. Exposure and the stream buffers are set before the
        // frame rate because the rate the camera will accept depends on them, and
        // setCameraFrameRate clamps against what the camera reports at that moment.
        setExposureTimeLowerLimit(settings.exposure_lower_limit_us);
        setStrobeLineToOutput(settings.strobe_line);
        setStreamBufferCount(settings.stream_buffers);

        // Assigning the result back to this->FPS matters: the member is what gets
        // written into the metadata and reused on recovery, so it has to hold the
        // rate actually achieved rather than the one requested.
        FPS = setCameraFrameRate(settings.fps);

        INodeMap& nodeMap = pCam->GetNodeMap();

        CEnumerationPtr ptrAcquisitionMode = nodeMap.GetNode("AcquisitionMode");
        if (!IsReadable(ptrAcquisitionMode) || !IsWritable(ptrAcquisitionMode)) {
            throw runtime_error("Unable to set acquisition mode to continuous");
        }

        CEnumEntryPtr ptrAcquisitionModeContinuous = ptrAcquisitionMode->GetEntryByName("Continuous");
        if (!IsReadable(ptrAcquisitionModeContinuous)) {
            throw runtime_error("Unable to set acquisition mode to continuous");
        }
        ptrAcquisitionMode->SetIntValue(ptrAcquisitionModeContinuous->GetValue());

        pCam->BeginAcquisition();
        acquiring = true;

        imageWidth = static_cast<size_t>(pCam->Width.GetValue());
        imageHeight = static_cast<size_t>(pCam->Height.GetValue());
        pixelFormat = string(pCam->PixelFormat.GetCurrentEntry()->GetSymbolic().c_str());

        if (settings.recording_mode == "video") {
            // The container extension is appended by the sink, since it comes from
            // the encoder settings.
            const fs::path stem = fs::path(path) / (start_time + "_" + mouse_ID + "_video");
            sink = behaviour_camera::makeVideoSink(stem, settings.video,
                                                   static_cast<int>(imageWidth),
                                                   static_cast<int>(imageHeight), FPS);
        } else if (settings.recording_mode == "raw") {
            const fs::path binFilename =
                fs::path(path) / (start_time + "_" + mouse_ID + "_binary_video.bin");
            sink = behaviour_camera::makeRawSink(binFilename);
        } else {
            throw runtime_error("Unknown recording mode '" + settings.recording_mode +
                                "'. Use \"raw\" or \"video\".");
        }

        cout << "Recording rig " << rig << " from camera " << camSerial << ": "
             << imageWidth << "x" << imageHeight << " " << pixelFormat
             << " at " << FPS << " fps" << endl;
        cout << "Writing " << sink->describe() << endl;
    }

    // Every Spinnaker call here can throw, and a destructor is implicitly noexcept:
    // an escaping exception calls std::terminate, which showed up as the process
    // dying with 0xC0000409 at the end of every otherwise successful session.
    ~CameraRecorder()
    {
        try {
            if (pCam) {
                if (acquiring) {
                    pCam->EndAcquisition();
                    acquiring = false;
                }
                pCam->DeInit();
            }
        }
        catch (Spinnaker::Exception& e) {
            cerr << "Warning: error shutting the camera down: " << e.what() << endl;
        }

        // The system instance must outlive every camera reference.
        pCam = nullptr;

        try {
            if (spinSystem) {
                spinSystem->ReleaseInstance();
            }
        }
        catch (Spinnaker::Exception& e) {
            cerr << "Warning: error releasing the Spinnaker system: " << e.what() << endl;
        }
    }

    // True if the session ran to a normal stop, false if it was cut short.
    bool startRecording(bool show_frame, bool save_video)
    {
        frame_IDs.clear();
        saveData();

        bool completed = captureFrames(show_frame, save_video);

        // The whole analysis chain assumes output frame i is the i-th entry of
        // frame_IDs. Checking it here, while the session is still in hand, is the
        // difference between a known-bad recording and one that is discovered to be
        // misaligned months later.
        outputFrameCount = sink ? sink->frameCount() : -1;
        if (outputFrameCount >= 0 &&
            outputFrameCount != static_cast<int64_t>(frame_IDs_mem.size())) {
            cerr << "Error: the output holds " << outputFrameCount << " frames but "
                 << frame_IDs_mem.size() << " frame IDs were recorded. "
                 << "They must match for frames to be mapped back to their timestamps."
                 << endl;
            abortReason = "output frame count does not match the number of frame IDs";
            completed = false;
        }

        end_time = currentDateTime();
        completedNormally = completed;
        saveData();

        reportSummary();

        // Written whatever the outcome, because the launcher waits for it and would
        // otherwise hang forever on a failed session. The metadata records whether
        // the session actually finished cleanly.
        createSignalFile();
        return completed;
    }

private:
    string mouse_ID;
    string start_time;
    string end_time;
    string path;
    string camSerial;
    Settings settings;
    string rig;
    double FPS;
    int windowWidth;
    int windowHeight;
    size_t frame_count;

    bool acquiring = false;
    bool completedNormally = false;
    string abortReason;

    CameraPtr pCam;
    SystemPtr spinSystem;

    vector<uint64_t> frame_IDs;       // pending, flushed to the backup file in batches
    vector<uint64_t> frame_IDs_mem;   // every ID, written into the metadata at the end
    string title;
    size_t imageWidth = 0;
    size_t imageHeight = 0;
    string pixelFormat;
    unique_ptr<FrameSink> sink;

    // Frames the camera produced that never reached us, counted from gaps in the
    // camera's own frame counter. Recorded so a session says plainly whether it lost
    // anything, rather than leaving it to be discovered during analysis.
    int64_t droppedFrames = 0;
    int64_t incompleteFrames = 0;
    uint64_t lastFrameID = 0;
    bool haveLastFrameID = false;

    static constexpr size_t kFrameIdFlushInterval = 200;
    static constexpr auto kStopSignalCheckInterval = milliseconds(250);
    static constexpr auto kMetadataSaveInterval = seconds(30);

    // A single incomplete frame is ordinary packet loss and is not worth reacting to:
    // re-initialising the camera over one costs seconds of recording and resets the
    // frame-ID counter. Only a sustained run of failures means something is actually
    // wrong.
    static constexpr int kConsecutiveFailuresBeforeRecovery = 10;
    int consecutiveFailures = 0;

    int recoveryAttempts = 0;
    static constexpr int kMaxRecoveryAttempts = 3;
    static constexpr auto kRecoveryCooldown = seconds(5);

    bool captureFrames(bool show_frame, bool save_video)
    {
        const fs::path frameIdPath =
            fs::path(path) / (start_time + "_" + mouse_ID + "_frame_ids_backup.txt");
        ofstream frameIDFile(frameIdPath, ios_base::app);
        if (!frameIDFile.is_open()) {
            abortReason = "could not open the frame ID backup file";
            cerr << "Error: " << abortReason << endl;
            return false;
        }

        GLFWwindow* window = nullptr;
        if (show_frame) {
            window = createWindow();
            if (!window) {
                abortReason = "could not create the preview window";
                return false;
            }
        }

        const auto displayInterval =
            milliseconds(settings.display_fps > 0 ? 1000 / settings.display_fps : 33);
        auto lastDisplay = steady_clock::now();
        auto lastStopCheck = steady_clock::now();
        auto lastMetadataSave = steady_clock::now();

        bool keepRunning = true;
        bool completed = true;

        while (keepRunning) {
            try {
                ScopedImage frame(pCam->GetNextImage(1000));

                if (!frame.get().IsValid() || frame.get()->IsIncomplete()) {
                    ++incompleteFrames;
                    ++consecutiveFailures;

                    if (consecutiveFailures < kConsecutiveFailuresBeforeRecovery) {
                        // Drop it and carry on; the next frame is usually fine.
                        continue;
                    }

                    cerr << consecutiveFailures << " consecutive bad frames; "
                         << "attempting camera recovery." << endl;
                    if (!attemptRecovery()) {
                        abortReason = "camera could not be recovered";
                        cerr << "Error: " << abortReason << ". Stopping recording." << endl;
                        completed = false;
                        break;
                    }
                    consecutiveFailures = 0;
                    continue;
                }

                consecutiveFailures = 0;
                countDroppedFrames(frame.get()->GetFrameID());

                if (save_video && !writeFrame(frame.get(), frameIDFile)) {
                    abortReason = "failed to write image data to the binary file";
                    cerr << "Error: " << abortReason << endl;
                    completed = false;
                    break;
                }

                const auto now = steady_clock::now();

                if (show_frame && now - lastDisplay >= displayInterval) {
                    drawPreview(window, frame.get());
                    lastDisplay = now;
                }

                // Pumped every iteration rather than only when a frame is drawn, so
                // the window stays responsive and Esc is seen promptly.
                if (show_frame) {
                    glfwPollEvents();
                    if (glfwGetKey(window, GLFW_KEY_ESCAPE) == GLFW_PRESS ||
                        glfwWindowShouldClose(window)) {
                        cout << "Stop requested from the preview window." << endl;
                        keepRunning = false;
                    }
                }

                // On a wall-clock timer, and outside the preview branch. It used to
                // be inside it and gated on a frame counter, so with the preview off
                // there was no way to stop the recorder at all.
                if (now - lastStopCheck >= kStopSignalCheckInterval) {
                    lastStopCheck = now;
                    if (stopSignalPresent()) {
                        cout << "Stop signal file found." << endl;
                        keepRunning = false;
                    }
                }

                // Keeps the metadata usable if the machine dies mid-session.
                if (now - lastMetadataSave >= kMetadataSaveInterval) {
                    lastMetadataSave = now;
                    saveData();
                }

                ++frame_count;
            }
            catch (Spinnaker::Exception& e) {
                ++consecutiveFailures;
                cerr << "Camera error: " << e.what() << endl;

                if (consecutiveFailures < kConsecutiveFailuresBeforeRecovery) {
                    continue;
                }
                if (!attemptRecovery()) {
                    abortReason = string("unrecoverable camera error: ") + e.what();
                    cerr << "Error: stopping recording." << endl;
                    completed = false;
                    break;
                }
                consecutiveFailures = 0;
            }
        }

        if (pCam && acquiring) {
            try {
                pCam->EndAcquisition();
            }
            catch (Spinnaker::Exception& e) {
                cerr << "Warning: error ending acquisition: " << e.what() << endl;
            }
            acquiring = false;
        }

        flushFrameIds(frameIDFile);
        frameIDFile.close();

        // Closing the sink is what makes an encoder flush and finalise its
        // container, so a failure here means the output is not trustworthy even if
        // every frame was handed over successfully.
        if (!sink->finish()) {
            if (abortReason.empty()) {
                abortReason = "the output could not be closed cleanly";
            }
            completed = false;
        }

        if (window) {
            glfwDestroyWindow(window);
            glfwTerminate();
        }

        return completed;
    }

    GLFWwindow* createWindow()
    {
        if (!glfwInit()) {
            cerr << "Error: failed to initialise GLFW" << endl;
            return nullptr;
        }

        GLFWwindow* window = glfwCreateWindow(windowWidth, windowHeight, title.c_str(), nullptr, nullptr);
        if (!window) {
            cerr << "Error: failed to create the preview window" << endl;
            glfwTerminate();
            return nullptr;
        }

        glfwMakeContextCurrent(window);
        glViewport(0, 0, windowWidth, windowHeight);
        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        return window;
    }

    void drawPreview(GLFWwindow* window, const ImagePtr& image)
    {
        cv::Mat source(cv::Size(static_cast<int>(imageWidth), static_cast<int>(imageHeight)),
                       CV_8UC1, image->GetData(), image->GetStride());

        cv::Mat resized;
        cv::resize(source, resized, cv::Size(windowWidth, windowHeight));

        glClear(GL_COLOR_BUFFER_BIT);
        glPixelZoom(1.0f, -1.0f);
        glRasterPos2i(-1, 1);
        glDrawPixels(resized.cols, resized.rows, GL_LUMINANCE, GL_UNSIGNED_BYTE, resized.data);
        glfwSwapBuffers(window);
    }

    bool writeFrame(const ImagePtr& image, ofstream& frameIDFile)
    {
        if (!sink->write(image->GetData(), image->GetImageSize())) {
            return false;
        }

        const uint64_t frameID = image->GetFrameID();
        frame_IDs.push_back(frameID);
        frame_IDs_mem.push_back(frameID);

        if (frame_IDs.size() >= kFrameIdFlushInterval) {
            flushFrameIds(frameIDFile);
        }
        return true;
    }

    void flushFrameIds(ofstream& frameIDFile)
    {
        if (frame_IDs.empty()) {
            return;
        }
        for (const uint64_t id : frame_IDs) {
            frameIDFile << id << '\n';
        }
        frameIDFile.flush();
        frame_IDs.clear();
    }

    // The camera's frame counter increments for every frame it produces, including
    // ones lost in transfer, so a gap is a dropped frame.
    void countDroppedFrames(uint64_t frameID)
    {
        if (haveLastFrameID && frameID > lastFrameID + 1) {
            droppedFrames += static_cast<int64_t>(frameID - lastFrameID - 1);
        }
        lastFrameID = frameID;
        haveLastFrameID = true;
    }

    bool attemptRecovery()
    {
        if (recoveryAttempts >= kMaxRecoveryAttempts) {
            cerr << "Maximum recovery attempts reached; the camera fault persists." << endl;
            return false;
        }

        try {
            cerr << "Attempting camera recovery (" << recoveryAttempts + 1 << " of "
                 << kMaxRecoveryAttempts << ")..." << endl;

            if (acquiring) {
                pCam->EndAcquisition();
                acquiring = false;
            }
            std::this_thread::sleep_for(milliseconds(500));

            // This resets the camera's frame-ID counter, so IDs after a recovery are
            // no longer a continuous index. The metadata records that it happened.
            pCam->DeInit();
            std::this_thread::sleep_for(milliseconds(500));

            pCam->Init();
            setExposureTimeLowerLimit(settings.exposure_lower_limit_us);
            setStrobeLineToOutput(settings.strobe_line);
            setStreamBufferCount(settings.stream_buffers);
            setCameraFrameRate(FPS);

            pCam->BeginAcquisition();
            acquiring = true;
            ++cameraResets;
            haveLastFrameID = false;   // the counter restarted; do not count the jump

            ScopedImage test(pCam->GetNextImage(1000));
            if (test.get().IsValid() && !test.get()->IsIncomplete()) {
                cerr << "Camera recovered." << endl;
                recoveryAttempts = 0;
                return true;
            }

            ++recoveryAttempts;
            std::this_thread::sleep_for(kRecoveryCooldown);
            return false;
        }
        catch (Spinnaker::Exception& e) {
            cerr << "Recovery attempt failed: " << e.what() << endl;
            ++recoveryAttempts;
            return false;
        }
    }

    bool stopSignalPresent() const
    {
        error_code error;
        return fs::exists(fs::path(path) / ("stop_camera_" + rig + ".signal"), error);
    }

    void reportSummary() const
    {
        cout << "\nSession summary\n"
             << "  frames recorded   " << frame_IDs_mem.size() << "\n"
             << "  dropped in transit " << droppedFrames << "\n"
             << "  incomplete frames " << incompleteFrames << "\n"
             << "  camera resets     " << cameraResets << "\n"
             << "  finished normally " << (completedNormally ? "yes" : "no") << endl;
        if (!completedNormally && !abortReason.empty()) {
            cout << "  stopped because   " << abortReason << endl;
        }
    }

    void saveData()
    {
        json data;
        data["frame_rate"] = FPS;
        data["start_time"] = start_time;
        data["end_time"] = end_time;
        data["image_height"] = imageHeight;
        data["image_width"] = imageWidth;
        data["pixel_format"] = pixelFormat;
        data["frame_IDs"] = frame_IDs_mem;

        // Added alongside the original fields rather than replacing any, so existing
        // analysis code reads this file exactly as before.
        data["serial_number"] = camSerial;
        data["rig"] = rig;
        data["dropped_frames"] = droppedFrames;
        data["incomplete_frames"] = incompleteFrames;
        data["camera_resets"] = cameraResets;
        data["completed_normally"] = completedNormally;
        if (!abortReason.empty()) {
            data["abort_reason"] = abortReason;
        }
        data["recording_mode"] = settings.recording_mode;
        if (sink) {
            data["output_file"] = sink->path().filename().string();
        }
        if (outputFrameCount >= 0) {
            data["output_frame_count"] = outputFrameCount;
        }
        data["settings"] = settings.toJson();

        // Written to a temporary file and moved into place, so a crash partway
        // through cannot leave a truncated metadata file where a good one was.
        const fs::path finalPath =
            fs::path(path) / (start_time + "_" + mouse_ID + "_Tracker_data.json");
        const fs::path tempPath = finalPath.string() + ".tmp";

        {
            ofstream file(tempPath);
            if (!file) {
                cerr << "Warning: could not write " << finalPath << endl;
                return;
            }
            file << data.dump(4);
        }

        error_code error;
        fs::rename(tempPath, finalPath, error);
        if (error) {
            cerr << "Warning: could not replace " << finalPath << ": " << error.message() << endl;
        }
    }

    double setCameraFrameRate(double frameRate)
    {
        INodeMap& nodeMap = pCam->GetNodeMap();

        CBooleanPtr ptrFrameRateEnable = nodeMap.GetNode("AcquisitionFrameRateEnable");
        if (IsWritable(ptrFrameRateEnable)) {
            ptrFrameRateEnable->SetValue(true);
        }

        CFloatPtr ptrFrameRate = nodeMap.GetNode("AcquisitionFrameRate");
        if (!IsWritable(ptrFrameRate)) {
            throw runtime_error("Unable to set the frame rate on this camera");
        }

        // The achievable range depends on pixel format, ROI and exposure, so it is
        // read from the device rather than kept in a table that needs maintaining.
        const double minRate = ptrFrameRate->GetMin();
        const double maxRate = ptrFrameRate->GetMax();
        double applied = frameRate;
        if (applied < minRate) applied = minRate;
        if (applied > maxRate) applied = maxRate;

        if (applied != frameRate) {
            cout << "Requested " << frameRate << " fps; this camera allows " << minRate
                 << " to " << maxRate << ", so recording at " << applied << " fps." << endl;
        }

        ptrFrameRate->SetValue(applied);
        return ptrFrameRate->GetValue();   // the camera quantises it
    }

    // Decides how many frames the driver may hold while we are busy writing. The
    // camera default is 10, about a third of a second at 30 fps, which is why an
    // ordinary disk stall used to cost frames.
    void setStreamBufferCount(int count)
    {
        if (count <= 0) {
            return;
        }

        INodeMap& streamNodeMap = pCam->GetTLStreamNodeMap();

        CEnumerationPtr ptrBufferCountMode = streamNodeMap.GetNode("StreamBufferCountMode");
        if (IsWritable(ptrBufferCountMode)) {
            CEnumEntryPtr manual = ptrBufferCountMode->GetEntryByName("Manual");
            if (IsReadable(manual)) {
                ptrBufferCountMode->SetIntValue(manual->GetValue());
            }
        }

        CIntegerPtr ptrBufferCount = streamNodeMap.GetNode("StreamBufferCountManual");
        if (!IsWritable(ptrBufferCount)) {
            cerr << "Warning: this camera will not let the buffer count be set; "
                 << "leaving it at the default." << endl;
            return;
        }

        const int64_t requested = count;
        const int64_t clamped = std::min(std::max(requested, ptrBufferCount->GetMin()),
                                         ptrBufferCount->GetMax());
        ptrBufferCount->SetValue(clamped);

        if (clamped != requested) {
            cout << "Stream buffers: asked for " << requested << ", camera allows up to "
                 << ptrBufferCount->GetMax() << ", using " << clamped << "." << endl;
        }

        CIntegerPtr ptrBufferResult = streamNodeMap.GetNode("StreamBufferCountResult");
        const int64_t inUse = IsReadable(ptrBufferResult) ? ptrBufferResult->GetValue() : clamped;
        cout << "Stream buffers: " << inUse << " ("
             << (FPS > 0 ? inUse / FPS : 0.0) << " s of slack at the current rate)" << endl;
    }

    void setStrobeLineToOutput(int lineNumber)
    {
        INodeMap& nodeMap = pCam->GetNodeMap();
        const string lineName = "Line" + to_string(lineNumber);

        CEnumerationPtr ptrLineSelector = nodeMap.GetNode("LineSelector");
        if (!IsWritable(ptrLineSelector)) {
            throw runtime_error("Unable to access LineSelector");
        }
        CEnumEntryPtr ptrLine = ptrLineSelector->GetEntryByName(lineName.c_str());
        if (!IsReadable(ptrLine)) {
            throw runtime_error("Camera has no " + lineName + " to use as the frame strobe");
        }
        ptrLineSelector->SetIntValue(ptrLine->GetValue());

        CEnumerationPtr ptrLineMode = nodeMap.GetNode("LineMode");
        if (!IsWritable(ptrLineMode)) {
            throw runtime_error("Unable to access LineMode");
        }
        CEnumEntryPtr ptrOutput = ptrLineMode->GetEntryByName("Output");
        if (!IsReadable(ptrOutput)) {
            throw runtime_error("Unable to set " + lineName + " to output");
        }
        ptrLineMode->SetIntValue(ptrOutput->GetValue());
    }

    void setExposureTimeLowerLimit(double exposureTimeLowerLimit)
    {
        INodeMap& nodeMap = pCam->GetNodeMap();

        CEnumerationPtr ptrExposureAuto = nodeMap.GetNode("ExposureAuto");
        if (!IsWritable(ptrExposureAuto)) {
            throw runtime_error("Unable to access ExposureAuto");
        }
        CEnumEntryPtr ptrContinuous = ptrExposureAuto->GetEntryByName("Continuous");
        if (!IsReadable(ptrContinuous)) {
            throw runtime_error("Unable to set ExposureAuto to Continuous");
        }
        ptrExposureAuto->SetIntValue(ptrContinuous->GetValue());

        CFloatPtr ptrLowerLimit = nodeMap.GetNode("AutoExposureExposureTimeLowerLimit");
        if (!IsReadable(ptrLowerLimit) || !IsWritable(ptrLowerLimit)) {
            throw runtime_error("Unable to access AutoExposureExposureTimeLowerLimit");
        }

        const double lowest = ptrLowerLimit->GetMin();
        const double highest = ptrLowerLimit->GetMax();
        double applied = exposureTimeLowerLimit;
        if (applied < lowest) applied = lowest;
        if (applied > highest) applied = highest;
        ptrLowerLimit->SetValue(applied);
    }

    void createSignalFile() const
    {
        const fs::path signalFile = fs::path(path) / ("rig_" + rig + "_camera_finished.signal");
        ofstream file(signalFile);
    }

    int cameraResets = 0;
    int64_t outputFrameCount = -1;
};

namespace {

void printUsage()
{
    cout <<
        "behaviour_camera - records from a Teledyne/FLIR camera\n"
        "\n"
        "Every setting has a working default, so only --serial_number is required.\n"
        "The options below override those defaults for one run; the rig launcher\n"
        "fills them in from each rig's camera section in rigs.yaml.\n"
        "\n"
        "Session\n"
        "  --serial_number <id>   camera to record from (required)\n"
        "  --id <name>            subject ID, used in filenames (default NoID)\n"
        "  --date <stamp>         date stamp for filenames (default: now)\n"
        "  --path <dir>           output directory, created if missing\n"
        "  --rig <name>           rig name used in the signal filenames\n"
        "\n"
        "Camera\n"
        "  --fps <rate>           frame rate; clamped to what the camera allows\n"
        "  --exposure-min <us>    auto-exposure floor in microseconds\n"
        "  --stream-buffers <n>   frames the driver may hold while writing\n"
        "  --strobe-line <n>      GPIO line pulsed once per frame\n"
        "\n"
        "Recording\n"
        "  --mode raw|video       .bin of raw frames, or GPU-encoded video\n"
        "  --qp <n>               encoder quality, lower is better (video mode)\n"
        "  --gop <n>              frames between keyframes (video mode)\n"
        "\n"
        "Preview\n"
        "  --windowWidth <px>     preview width\n"
        "  --windowHeight <px>    preview height\n"
        "  --no-preview           record without a preview window\n"
        "\n"
        "  --help                 this message\n"
        "\n"
        "Run camera_probe to see which cameras are attached.\n";
}

// Only what was actually asked for. Anything left unset keeps the default from
// Settings, so an option that is not passed cannot change behaviour.
struct Arguments
{
    string mouse_ID = "NoID";
    string date_time;
    string path;
    string serial_number;
    string mode;
    string rig;
    optional<double> fps;
    optional<double> exposureMin;
    optional<int> streamBuffers;
    optional<int> strobeLine;
    optional<int> qp;
    optional<int> gop;
    optional<int> windowWidth;
    optional<int> windowHeight;
    bool showPreview = true;
};

// Returns false if the program should stop: either --help, or an argument that could
// not be understood. Unknown arguments are rejected rather than ignored, because the
// old parser stepped two at a time and silently mis-read everything after a flag that
// had no value.
bool parseArguments(int argc, char** argv, Arguments& args, int& exitCode)
{
    for (int i = 1; i < argc; ++i) {
        const string arg = argv[i];

        if (arg == "--help" || arg == "-h") {
            printUsage();
            exitCode = 0;
            return false;
        }
        if (arg == "--no-preview") {
            args.showPreview = false;
            continue;
        }

        if (i + 1 >= argc) {
            cerr << "Error: " << arg << " needs a value.\n";
            exitCode = 2;
            return false;
        }
        const string value = argv[++i];

        try {
            if (arg == "--id") args.mouse_ID = value;
            else if (arg == "--date") args.date_time = value;
            else if (arg == "--path") args.path = value;
            else if (arg == "--serial_number") args.serial_number = value;
            else if (arg == "--mode") args.mode = value;
            else if (arg == "--rig") args.rig = value;
            else if (arg == "--fps") args.fps = stod(value);
            else if (arg == "--exposure-min") args.exposureMin = stod(value);
            else if (arg == "--stream-buffers") args.streamBuffers = stoi(value);
            else if (arg == "--strobe-line") args.strobeLine = stoi(value);
            else if (arg == "--qp") args.qp = stoi(value);
            else if (arg == "--gop") args.gop = stoi(value);
            else if (arg == "--windowWidth") args.windowWidth = stoi(value);
            else if (arg == "--windowHeight") args.windowHeight = stoi(value);
            else {
                cerr << "Error: unrecognised argument " << arg << "\n"
                     << "Run with --help to see the accepted options.\n";
                exitCode = 2;
                return false;
            }
        }
        catch (const std::exception&) {
            // stod/stoi throw on anything non-numeric; this used to escape main and
            // surface as an unhandled exception.
            cerr << "Error: " << arg << " expects a number, got '" << value << "'\n";
            exitCode = 2;
            return false;
        }
    }

    return true;
}

} // namespace

int main(int argc, char** argv)
{
    Arguments args;
    int exitCode = 0;
    if (!parseArguments(argc, argv, args, exitCode)) {
        return exitCode;
    }

    if (args.serial_number.empty()) {
        cerr << "Error: --serial_number is required.\n"
             << "Run camera_probe to see which cameras are attached, "
             << "or behaviour_camera --help.\n";
        return 2;
    }

    if (args.date_time.empty()) {
        args.date_time = currentDateTime();
    }

    // Defaults from the code, then whatever this run asked for. Nothing is read
    // from disk, so there is no second place for camera settings to live and
    // disagree with the rig configuration.
    Settings settings;

    // The rig name matters more than it looks: the launcher watches for
    // rig_<name>_camera_finished.signal and writes stop_camera_<name>.signal, and
    // it knows the name from its own configuration. Taking it from the launcher
    // means the two cannot disagree. Falling back to the serial keeps a bench
    // camera working with no arguments at all.
    settings.rig = args.rig.empty() ? ("cam_" + args.serial_number) : args.rig;

    if (!args.mode.empty())   settings.recording_mode = args.mode;
    if (args.fps)             settings.fps = *args.fps;
    if (args.exposureMin)     settings.exposure_lower_limit_us = *args.exposureMin;
    if (args.streamBuffers)   settings.stream_buffers = *args.streamBuffers;
    if (args.strobeLine)      settings.strobe_line = *args.strobeLine;
    if (args.qp)              settings.video.qp = *args.qp;
    if (args.gop)             settings.video.gop = *args.gop;
    if (args.windowWidth)     settings.window_width = *args.windowWidth;
    if (args.windowHeight)    settings.window_height = *args.windowHeight;

    string path = args.path;
    if (path.empty()) {
        path = (fs::path(settings.output_root) / (args.date_time + "_" + args.mouse_ID)).string();
    }

    // Created whichever way the path was arrived at. A path given with --path used to
    // be left alone, so the only sign that it did not exist was the binary file
    // failing to open several steps later.
    try {
        fs::create_directories(path);
    }
    catch (const fs::filesystem_error& e) {
        cerr << "Error: unable to create the output directory " << path << ": "
             << e.what() << endl;
        return 2;
    }
    if (!fs::is_directory(path)) {
        cerr << "Error: output path is not a directory: " << path << endl;
        return 2;
    }

    // From here on, everything printed also lands in the session folder, which is the
    // only record of what happened on a rig launched without a console.
    SessionLog log(fs::path(path) / (args.date_time + "_" + args.mouse_ID + "_camera_log.txt"));

    try {
        CameraRecorder camera(args.mouse_ID, args.date_time, path, args.serial_number, settings);
        const bool completed = camera.startRecording(args.showPreview, true);
        // Non-zero on an aborted session so a launcher can tell the difference. The
        // finished-signal file is written either way.
        return completed ? 0 : 1;
    }
    catch (const std::exception& e) {
        cerr << "Error: " << e.what() << endl;
        return 2;
    }
}
