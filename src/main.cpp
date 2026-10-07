// Standard library includes
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

// Third-party library includes
#include <nlohmann/json.hpp>

// Spinnaker SDK includes
#include <Spinnaker.h>
#include <SpinGenApi/SpinnakerGenApi.h>

#include "config.h"
#include "frame_pipeline.h"
#include "frame_sink.h"
#include "preview.h"

using namespace Spinnaker;
using namespace Spinnaker::GenApi;
using namespace Spinnaker::GenICam;
using namespace std;
using namespace std::chrono;
using json = nlohmann::json;
namespace fs = std::filesystem;
using behaviour_camera::Settings;
using behaviour_camera::FrameSink;
using behaviour_camera::Frame;
using behaviour_camera::FramePool;
using behaviour_camera::FrameQueue;
using behaviour_camera::PreviewSlot;
using behaviour_camera::Preview;
using behaviour_camera::PreviewStatus;

namespace {

// Returns a Spinnaker image to the driver however the scope is left, including by
// an exception. A frame that is not released is never reused, so a handful of
// leaked frames starves the camera.
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
            // Nothing useful to do from a destructor, and throwing would terminate.
        }
    }

    ScopedImage(const ScopedImage&) = delete;
    ScopedImage& operator=(const ScopedImage&) = delete;

    ImagePtr& get() { return image_; }

private:
    ImagePtr image_;
};

// Sends everything written to a stream to a second one as well, so console output
// is also captured in a session log. Rig machines are launched without a console,
// where otherwise every diagnostic is lost precisely when it is wanted.
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
        // Two threads can log at once now, so serialise here rather than letting
        // their characters interleave into unreadable output.
        std::lock_guard<std::mutex> lock(mutex_);
        const int a = first_->sputc(static_cast<char>(ch));
        const int b = second_->sputc(static_cast<char>(ch));
        return (a == traits_type::eof() || b == traits_type::eof()) ? traits_type::eof() : ch;
    }

    int sync() override
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const int a = first_->pubsync();
        const int b = second_->pubsync();
        return (a == 0 && b == 0) ? 0 : -1;
    }

private:
    std::mutex mutex_;
    std::streambuf* first_;
    std::streambuf* second_;
};

class SessionLog
{
public:
    explicit SessionLog(const fs::path& logPath)
    {
        file_.open(logPath, ios::out | ios::app);
        if (!file_.is_open()) {
            cerr << "Warning: could not open the session log " << logPath << endl;
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
          FPS(settings.fps)
    {
        spinSystem = System::GetInstance();
        CameraList camList = spinSystem->GetCameras();
        pCam = camList.GetBySerial(camSerial);
        camList.Clear();

        if (!pCam) {
            throw runtime_error("No camera with serial " + camSerial +
                                ". Run camera_probe to see which cameras are attached.");
        }

        pCam->Init();

        // Exposure and the stream buffers go first because the frame rate the
        // camera will accept depends on them, and setCameraFrameRate clamps
        // against what the camera reports at that moment.
        setExposureTimeLowerLimit(settings.exposure_lower_limit_us);
        setStrobeLineToOutput(settings.strobe_line);
        setStreamBufferCount(settings.stream_buffers);
        FPS = setCameraFrameRate(settings.fps);

        INodeMap& nodeMap = pCam->GetNodeMap();
        CEnumerationPtr ptrAcquisitionMode = nodeMap.GetNode("AcquisitionMode");
        if (!IsReadable(ptrAcquisitionMode) || !IsWritable(ptrAcquisitionMode)) {
            throw runtime_error("Unable to set acquisition mode to continuous");
        }
        CEnumEntryPtr ptrContinuous = ptrAcquisitionMode->GetEntryByName("Continuous");
        if (!IsReadable(ptrContinuous)) {
            throw runtime_error("Unable to set acquisition mode to continuous");
        }
        ptrAcquisitionMode->SetIntValue(ptrContinuous->GetValue());

        pCam->BeginAcquisition();
        acquiring = true;

        imageWidth = static_cast<size_t>(pCam->Width.GetValue());
        imageHeight = static_cast<size_t>(pCam->Height.GetValue());
        pixelFormat = string(pCam->PixelFormat.GetCurrentEntry()->GetSymbolic().c_str());
        frameBytes = readPayloadSize();

        if (settings.recording_mode == "video") {
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

        checkDiskSpaceAtStart();
    }

    // Every Spinnaker call here can throw, and a destructor is implicitly noexcept:
    // an escaping exception calls std::terminate, which used to show up as the
    // process dying with 0xC0000409 at the end of an otherwise good session.
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

        pCam = nullptr;   // must be gone before the system is released

        try {
            if (spinSystem) {
                spinSystem->ReleaseInstance();
            }
        }
        catch (Spinnaker::Exception& e) {
            cerr << "Warning: error releasing the Spinnaker system: " << e.what() << endl;
        }
    }

    bool startRecording(bool showPreview, bool saveVideo)
    {
        saveVideo_ = saveVideo;
        saveData();

        // One pool of preallocated buffers, sized from the configured ring. This
        // is the slack that absorbs a disk stall: at 1.25 MiB a frame, a gigabyte
        // is about 800 frames, which is nearly half a minute at 30 fps.
        const size_t poolFrames = std::max<size_t>(
            8, (static_cast<size_t>(settings.ring_buffer_mb) * 1024u * 1024u) / frameBytes);
        pool = make_unique<FramePool>(frameBytes, poolFrames);
        previewSlot = make_unique<PreviewSlot>(frameBytes);

        cout << "Ring buffer: " << poolFrames << " frames ("
             << (poolFrames * frameBytes) / (1024 * 1024) << " MB, "
             << (FPS > 0 ? poolFrames / FPS : 0.0) << " s of slack)" << endl;

        const fs::path frameIdPath =
            fs::path(path) / (start_time + "_" + mouse_ID + "_frame_ids_backup.txt");
        frameIdFile.open(frameIdPath, ios_base::app);
        if (!frameIdFile.is_open()) {
            abortReason = "could not open the frame ID backup file";
            cerr << "Error: " << abortReason << endl;
            return false;
        }

        unique_ptr<Preview> preview;
        if (showPreview) {
            try {
                preview = make_unique<Preview>(
                    settings.window_width, settings.window_height,
                    "Rig " + rig + " - " + mouse_ID,
                    static_cast<int>(imageWidth), static_cast<int>(imageHeight),
                    settings.show_histogram);
            }
            catch (const std::exception& e) {
                // A missing display should not stop a recording; it is only the
                // preview. Say so and carry on headless.
                cerr << "Warning: no preview (" << e.what() << "). Recording anyway." << endl;
            }
        }

        sessionStart = steady_clock::now();
        thread captureThread(&CameraRecorder::captureLoop, this);
        thread writerThread(&CameraRecorder::writerLoop, this);

        displayLoop(preview.get());

        // Stop in order: capture first so no new frames arrive, then let the
        // writer drain what is already queued, so the last frames of a session
        // are never thrown away.
        stopRequested.store(true);
        captureThread.join();
        queue.close();
        writerThread.join();

        frameIdFile.close();

        bool completed = !failed.load();
        if (!sink->finish()) {
            if (abortReason.empty()) {
                abortReason = "the output could not be closed cleanly";
            }
            completed = false;
        }

        // The whole analysis chain assumes output frame i is the i-th entry of
        // frame_IDs. Checking it here, while the session is still in hand, is the
        // difference between a known-bad recording and one discovered to be
        // misaligned months later.
        outputFrameCount = sink->frameCount();
        {
            lock_guard<mutex> lock(frameIdMutex);
            if (outputFrameCount >= 0 &&
                outputFrameCount != static_cast<int64_t>(frameIDs.size())) {
                cerr << "Error: the output holds " << outputFrameCount << " frames but "
                     << frameIDs.size() << " frame IDs were recorded. They must match "
                     << "for frames to be mapped back to their timestamps." << endl;
                abortReason = "output frame count does not match the number of frame IDs";
                completed = false;
            }
        }

        end_time = currentDateTime();
        completedNormally = completed;
        saveData();
        reportSummary();

        // Written whatever the outcome, because the launcher waits for it and
        // would otherwise hang forever. The metadata says whether it went well.
        createSignalFile();
        return completed;
    }

private:
    // ---------------------------------------------------------------- threads

    // Does as little as possible: grab, copy, hand the camera's buffer straight
    // back, publish. Everything slower happens on another thread.
    void captureLoop()
    {
        int consecutiveFailures = 0;

        while (!stopRequested.load() && !failed.load()) {
            try {
                ScopedImage frame(pCam->GetNextImage(1000));

                if (!frame.get().IsValid() || frame.get()->IsIncomplete()) {
                    counters.incompleteFrames.fetch_add(1);
                    ++consecutiveFailures;

                    // One incomplete frame is ordinary packet loss. Re-initialising
                    // the camera over it costs seconds of recording and resets the
                    // frame-ID counter, so only a sustained run of failures counts
                    // as something actually being wrong.
                    if (consecutiveFailures < kConsecutiveFailuresBeforeRecovery) {
                        continue;
                    }
                    cerr << consecutiveFailures << " consecutive bad frames; "
                         << "attempting camera recovery." << endl;
                    if (!attemptRecovery()) {
                        fail("camera could not be recovered");
                        return;
                    }
                    consecutiveFailures = 0;
                    continue;
                }

                consecutiveFailures = 0;

                const auto arrival = steady_clock::now();
                if (haveLastArrival) {
                    intervals.add(duration<double, std::milli>(arrival - lastArrival).count());
                }
                lastArrival = arrival;
                haveLastArrival = true;

                const uint64_t frameID = frame.get()->GetFrameID();
                countDroppedInTransit(frameID);
                counters.framesCaptured.fetch_add(1);

                const void* data = frame.get()->GetData();
                const size_t size = frame.get()->GetImageSize();

                if (previewSlot->wanted()) {
                    previewSlot->publish(data, size, frameID);
                }

                if (!saveVideo_) {
                    continue;
                }

                Frame* buffer = pool->acquire();
                if (buffer == nullptr) {
                    // Every buffer is in flight, so the writer is behind. Dropping
                    // deliberately and counting it is better than blocking here,
                    // which would stall the camera and lose frames anyway - only
                    // silently, at the driver.
                    counters.droppedNoBuffer.fetch_add(1);
                    continue;
                }

                std::memcpy(buffer->data.data(), data, size);
                buffer->size = size;
                buffer->frameID = frameID;
                queue.push(buffer);
            }
            catch (Spinnaker::Exception& e) {
                ++consecutiveFailures;
                cerr << "Camera error: " << e.what() << endl;
                if (consecutiveFailures < kConsecutiveFailuresBeforeRecovery) {
                    continue;
                }
                if (!attemptRecovery()) {
                    fail(string("unrecoverable camera error: ") + e.what());
                    return;
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
    }

    // Owns the output and the frame-ID record. Frames appear in frame_IDs only
    // once written, and in write order, which is what keeps output frame i and
    // frame_IDs[i] the same thing even when frames have been dropped.
    void writerLoop()
    {
        while (Frame* frame = queue.pop()) {
            if (!sink->write(frame->data.data(), frame->size)) {
                pool->release(frame);
                fail("failed to write image data");
                return;
            }

            counters.framesWritten.fetch_add(1);
            counters.bytesWritten.fetch_add(static_cast<int64_t>(frame->size));

            {
                lock_guard<mutex> lock(frameIdMutex);
                frameIDs.push_back(frame->frameID);
                pendingFrameIds.push_back(frame->frameID);
                if (pendingFrameIds.size() >= kFrameIdFlushInterval) {
                    flushFrameIdsLocked();
                }
            }

            pool->release(frame);
        }

        lock_guard<mutex> lock(frameIdMutex);
        flushFrameIdsLocked();
    }

    // Runs on the main thread, because that is where the window belongs on
    // Windows, and because keeping it out of the capture loop is what stops the
    // window freezing exactly when the camera is in trouble.
    void displayLoop(Preview* preview)
    {
        const auto displayInterval =
            milliseconds(settings.display_fps > 0 ? 1000 / settings.display_fps : 33);

        auto lastStopCheck = steady_clock::now();
        auto lastMetadataSave = steady_clock::now();
        auto lastDiskCheck = steady_clock::now();
        auto lastRateSample = steady_clock::now();
        int64_t lastFrameCount = 0;
        int64_t lastByteCount = 0;

        vector<uint8_t> image;
        uint64_t previewFrameID = 0;

        while (!stopRequested.load() && !failed.load()) {
            const auto now = steady_clock::now();

            if (now - lastRateSample >= milliseconds(500)) {
                const double seconds = duration<double>(now - lastRateSample).count();
                const int64_t frames = counters.framesCaptured.load();
                const int64_t bytes = counters.bytesWritten.load();
                measuredFps = (frames - lastFrameCount) / seconds;
                writeRateMBs = (bytes - lastByteCount) / seconds / 1e6;
                lastFrameCount = frames;
                lastByteCount = bytes;
                lastRateSample = now;
                readCameraHealth();
            }

            if (preview != nullptr) {
                previewSlot->request();
                previewSlot->take(image, previewFrameID);
                preview->render(image, buildStatus());
                if (preview->stopRequested()) {
                    cout << "Stop requested from the preview window." << endl;
                    break;
                }
            }

            // On a wall-clock timer, and outside the preview branch: with the
            // preview off there used to be no way to stop the recorder at all.
            if (now - lastStopCheck >= kStopSignalCheckInterval) {
                lastStopCheck = now;
                if (stopSignalPresent()) {
                    cout << "Stop signal file found." << endl;
                    break;
                }
            }

            if (now - lastMetadataSave >= kMetadataSaveInterval) {
                lastMetadataSave = now;
                saveData();
            }

            if (now - lastDiskCheck >= kDiskCheckInterval) {
                lastDiskCheck = now;
                if (!diskSpaceStillSufficient()) {
                    break;
                }
            }

            if (preview == nullptr) {
                // Nothing to draw, so there is no reason to spin.
                std::this_thread::sleep_for(milliseconds(50));
            } else {
                std::this_thread::sleep_for(displayInterval);
            }
        }
    }

    // ---------------------------------------------------------------- helpers

    // Read occasionally rather than every frame: these are slow register reads
    // over the link, and nobody needs the camera's temperature at 30 Hz.
    void readCameraHealth()
    {
        try {
            INodeMap& nodeMap = pCam->GetNodeMap();
            CFloatPtr exposure = nodeMap.GetNode("ExposureTime");
            if (IsReadable(exposure)) {
                lastExposure = exposure->GetValue();
            }
            CFloatPtr temperature = nodeMap.GetNode("DeviceTemperature");
            if (IsReadable(temperature)) {
                lastTemperature = temperature->GetValue();
            }
        }
        catch (Spinnaker::Exception&) {
            // Health readings are a nicety; losing them is not worth a fuss.
        }
    }

    PreviewStatus buildStatus() const
    {
        PreviewStatus status;
        status.rig = rig;
        status.mouseID = mouse_ID;
        status.sinkDescription = sink->describe();
        status.pixelFormat = pixelFormat;
        status.imageWidth = static_cast<int>(imageWidth);
        status.imageHeight = static_cast<int>(imageHeight);
        status.targetFps = FPS;
        status.measuredFps = measuredFps;
        status.elapsedSeconds = duration<double>(steady_clock::now() - sessionStart).count();
        status.writeMegabytesPerSecond = writeRateMBs;

        status.framesWritten = counters.framesWritten.load();
        status.droppedInTransit = counters.droppedInTransit.load();
        status.droppedNoBuffer = counters.droppedNoBuffer.load();
        status.incompleteFrames = counters.incompleteFrames.load();
        status.cameraResets = counters.cameraResets.load();

        status.bufferDepth = queue.depth();
        status.bufferCapacity = pool->capacity();
        status.bufferHighWater = queue.highWater();

        error_code error;
        const auto space = fs::space(path, error);
        if (!error) {
            status.diskFreeGigabytes = space.available / 1e9;
            if (writeRateMBs > 0.1) {
                status.diskSecondsRemaining = (space.available / 1e6) / writeRateMBs;
            }
        }

        status.exposureMicroseconds = lastExposure;
        status.temperatureCelsius = lastTemperature;
        intervals.snapshot(status.intervalMedianMs, status.intervalP99Ms,
                           status.intervalWorstMs);
        return status;
    }

    size_t readPayloadSize() const
    {
        INodeMap& nodeMap = pCam->GetNodeMap();
        CIntegerPtr payload = nodeMap.GetNode("PayloadSize");
        if (IsReadable(payload)) {
            return static_cast<size_t>(payload->GetValue());
        }
        // Mono8 fallback; only reached if the camera will not report its own
        // payload size, in which case this is the best guess available.
        return imageWidth * imageHeight;
    }

    void checkDiskSpaceAtStart()
    {
        error_code error;
        const auto space = fs::space(path, error);
        if (error) {
            return;
        }

        const double bytesPerSecond = estimatedBytesPerSecond();
        if (bytesPerSecond <= 0) {
            return;
        }
        const double hours = space.available / bytesPerSecond / 3600.0;
        cout << "Disk: " << space.available / 1e9 << " GB free, roughly "
             << hours << " hours at this rate" << endl;
        if (hours < 0.5) {
            cerr << "Warning: less than 30 minutes of space on the capture drive." << endl;
        }
    }

    // Raw writes every byte; encoded output is far smaller, and this only feeds
    // the headroom warning, so a conservative guess is fine.
    double estimatedBytesPerSecond() const
    {
        const double raw = static_cast<double>(frameBytes) * FPS;
        return settings.recording_mode == "video" ? raw / 20.0 : raw;
    }

    // Stops gracefully while the output can still be finalised, rather than dying
    // with a write error partway through a trial.
    bool diskSpaceStillSufficient()
    {
        error_code error;
        const auto space = fs::space(path, error);
        if (error) {
            return true;
        }
        if (space.available > kDiskFloorBytes) {
            return true;
        }
        cerr << "Error: only " << space.available / 1e6
             << " MB left on the capture drive; stopping while the recording can "
             << "still be closed cleanly." << endl;
        {
            lock_guard<mutex> lock(reasonMutex);
            if (abortReason.empty()) {
                abortReason = "ran out of disk space";
            }
        }
        failed.store(true);
        return false;
    }

    void fail(const string& reason)
    {
        {
            lock_guard<mutex> lock(reasonMutex);
            if (abortReason.empty()) {
                abortReason = reason;
            }
        }
        failed.store(true);
        cerr << "Error: " << reason << ". Stopping recording." << endl;
    }

    void flushFrameIdsLocked()
    {
        for (const uint64_t id : pendingFrameIds) {
            frameIdFile << id << '\n';
        }
        frameIdFile.flush();
        pendingFrameIds.clear();
    }

    // The camera's counter increments for every frame it produces, including ones
    // lost in transfer, so a gap means a frame never reached us.
    void countDroppedInTransit(uint64_t frameID)
    {
        if (haveLastFrameID && frameID > lastFrameID + 1) {
            counters.droppedInTransit.fetch_add(
                static_cast<int64_t>(frameID - lastFrameID - 1));
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

            // This resets the camera's frame-ID counter, so IDs after a recovery
            // are no longer a continuous index. The metadata records that it
            // happened so the discontinuity is not a surprise later.
            pCam->DeInit();
            std::this_thread::sleep_for(milliseconds(500));

            pCam->Init();
            setExposureTimeLowerLimit(settings.exposure_lower_limit_us);
            setStrobeLineToOutput(settings.strobe_line);
            setStreamBufferCount(settings.stream_buffers);
            setCameraFrameRate(FPS);

            pCam->BeginAcquisition();
            acquiring = true;
            counters.cameraResets.fetch_add(1);
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

    void reportSummary()
    {
        lock_guard<mutex> lock(frameIdMutex);
        cout << "\nSession summary\n"
             << "  frames recorded        " << frameIDs.size() << "\n"
             << "  dropped in transit     " << counters.droppedInTransit.load() << "\n"
             << "  dropped, writer behind " << counters.droppedNoBuffer.load() << "\n"
             << "  incomplete frames      " << counters.incompleteFrames.load() << "\n"
             << "  camera resets          " << counters.cameraResets.load() << "\n"
             << "  ring buffer peak       " << queue.highWater() << " of "
             << (pool ? pool->capacity() : 0) << "\n"
             << "  finished normally      " << (completedNormally ? "yes" : "no") << endl;
        if (!completedNormally && !abortReason.empty()) {
            cout << "  stopped because        " << abortReason << endl;
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
        {
            lock_guard<mutex> lock(frameIdMutex);
            data["frame_IDs"] = frameIDs;
        }

        // Added alongside the original fields rather than replacing any, so
        // existing analysis code reads this file exactly as before.
        data["serial_number"] = camSerial;
        data["rig"] = rig;
        data["dropped_frames"] = counters.droppedInTransit.load();
        data["dropped_no_buffer"] = counters.droppedNoBuffer.load();
        data["incomplete_frames"] = counters.incompleteFrames.load();
        data["camera_resets"] = counters.cameraResets.load();
        data["ring_buffer_peak"] = queue.highWater();
        data["completed_normally"] = completedNormally;
        {
            lock_guard<mutex> lock(reasonMutex);
            if (!abortReason.empty()) {
                data["abort_reason"] = abortReason;
            }
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
            cerr << "Warning: could not replace " << finalPath << ": "
                 << error.message() << endl;
        }
    }

    double setCameraFrameRate(double frameRate)
    {
        INodeMap& nodeMap = pCam->GetNodeMap();
        CBooleanPtr ptrEnable = nodeMap.GetNode("AcquisitionFrameRateEnable");
        if (IsWritable(ptrEnable)) {
            ptrEnable->SetValue(true);
        }

        CFloatPtr ptrRate = nodeMap.GetNode("AcquisitionFrameRate");
        if (!IsWritable(ptrRate)) {
            throw runtime_error("Unable to set the frame rate on this camera");
        }

        // The achievable range depends on pixel format, ROI and exposure, so it is
        // read from the device rather than kept in a table needing maintenance.
        const double lo = ptrRate->GetMin();
        const double hi = ptrRate->GetMax();
        double applied = frameRate;
        if (applied < lo) applied = lo;
        if (applied > hi) applied = hi;
        if (applied != frameRate) {
            cout << "Requested " << frameRate << " fps; this camera allows " << lo
                 << " to " << hi << ", so recording at " << applied << " fps." << endl;
        }
        ptrRate->SetValue(applied);
        return ptrRate->GetValue();   // the camera quantises it
    }

    void setStreamBufferCount(int count)
    {
        if (count <= 0) {
            return;
        }
        INodeMap& stream = pCam->GetTLStreamNodeMap();

        CEnumerationPtr ptrMode = stream.GetNode("StreamBufferCountMode");
        if (IsWritable(ptrMode)) {
            CEnumEntryPtr manual = ptrMode->GetEntryByName("Manual");
            if (IsReadable(manual)) {
                ptrMode->SetIntValue(manual->GetValue());
            }
        }

        CIntegerPtr ptrCount = stream.GetNode("StreamBufferCountManual");
        if (!IsWritable(ptrCount)) {
            cerr << "Warning: this camera will not let the buffer count be set." << endl;
            return;
        }
        const int64_t clamped = std::min(std::max<int64_t>(count, ptrCount->GetMin()),
                                         ptrCount->GetMax());
        ptrCount->SetValue(clamped);

        CIntegerPtr ptrResult = stream.GetNode("StreamBufferCountResult");
        const int64_t inUse = IsReadable(ptrResult) ? ptrResult->GetValue() : clamped;
        cout << "Stream buffers: " << inUse << " ("
             << (FPS > 0 ? inUse / FPS : 0.0) << " s of slack at the current rate)" << endl;
    }

    void setStrobeLineToOutput(int lineNumber)
    {
        INodeMap& nodeMap = pCam->GetNodeMap();
        const string lineName = "Line" + to_string(lineNumber);

        CEnumerationPtr ptrSelector = nodeMap.GetNode("LineSelector");
        if (!IsWritable(ptrSelector)) {
            throw runtime_error("Unable to access LineSelector");
        }
        CEnumEntryPtr ptrLine = ptrSelector->GetEntryByName(lineName.c_str());
        if (!IsReadable(ptrLine)) {
            throw runtime_error("Camera has no " + lineName + " to use as the frame strobe");
        }
        ptrSelector->SetIntValue(ptrLine->GetValue());

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

    void setExposureTimeLowerLimit(double limit)
    {
        INodeMap& nodeMap = pCam->GetNodeMap();

        CEnumerationPtr ptrAuto = nodeMap.GetNode("ExposureAuto");
        if (!IsWritable(ptrAuto)) {
            throw runtime_error("Unable to access ExposureAuto");
        }
        CEnumEntryPtr ptrContinuous = ptrAuto->GetEntryByName("Continuous");
        if (!IsReadable(ptrContinuous)) {
            throw runtime_error("Unable to set ExposureAuto to Continuous");
        }
        ptrAuto->SetIntValue(ptrContinuous->GetValue());

        CFloatPtr ptrLimit = nodeMap.GetNode("AutoExposureExposureTimeLowerLimit");
        if (!IsReadable(ptrLimit) || !IsWritable(ptrLimit)) {
            throw runtime_error("Unable to access AutoExposureExposureTimeLowerLimit");
        }
        const double lo = ptrLimit->GetMin();
        const double hi = ptrLimit->GetMax();
        double applied = limit;
        if (applied < lo) applied = lo;
        if (applied > hi) applied = hi;
        ptrLimit->SetValue(applied);
    }

    void createSignalFile() const
    {
        const fs::path signalFile =
            fs::path(path) / ("rig_" + rig + "_camera_finished.signal");
        ofstream file(signalFile);
    }

    // ---------------------------------------------------------------- state

    string mouse_ID;
    string start_time;
    string end_time;
    string path;
    string camSerial;
    Settings settings;
    string rig;
    double FPS;

    CameraPtr pCam;
    SystemPtr spinSystem;
    bool acquiring = false;
    bool saveVideo_ = true;

    size_t imageWidth = 0;
    size_t imageHeight = 0;
    size_t frameBytes = 0;
    string pixelFormat;

    unique_ptr<FrameSink> sink;
    unique_ptr<FramePool> pool;
    unique_ptr<PreviewSlot> previewSlot;
    FrameQueue queue;
    behaviour_camera::Counters counters;

    atomic<bool> stopRequested{ false };
    atomic<bool> failed{ false };
    mutable mutex reasonMutex;
    string abortReason;
    bool completedNormally = false;
    int64_t outputFrameCount = -1;

    // frameIDs is the record that must match the output exactly, so only the
    // writer appends to it and everything else reads it under this mutex.
    mutable mutex frameIdMutex;
    vector<uint64_t> frameIDs;
    vector<uint64_t> pendingFrameIds;
    ofstream frameIdFile;

    behaviour_camera::IntervalStats intervals;
    steady_clock::time_point lastArrival;
    bool haveLastArrival = false;

    uint64_t lastFrameID = 0;
    bool haveLastFrameID = false;
    int recoveryAttempts = 0;

    steady_clock::time_point sessionStart;
    double measuredFps = 0.0;
    double writeRateMBs = 0.0;
    double lastExposure = -1.0;
    double lastTemperature = -1000.0;

    static constexpr size_t kFrameIdFlushInterval = 200;
    static constexpr auto kStopSignalCheckInterval = milliseconds(250);
    static constexpr auto kMetadataSaveInterval = seconds(30);
    static constexpr auto kDiskCheckInterval = seconds(10);
    static constexpr int kConsecutiveFailuresBeforeRecovery = 10;
    static constexpr int kMaxRecoveryAttempts = 3;
    static constexpr auto kRecoveryCooldown = seconds(5);
    static constexpr uint64_t kDiskFloorBytes = 2ull * 1024 * 1024 * 1024;
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
        "  --ring-buffer-mb <n>   RAM held between capture and writing\n"
        "\n"
        "Preview\n"
        "  --windowWidth <px>     preview width\n"
        "  --windowHeight <px>    preview height\n"
        "  --histogram            start with the exposure histogram showing\n"
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
    optional<int> ringBufferMb;
    optional<int> windowWidth;
    optional<int> windowHeight;
    bool showPreview = true;
    bool histogram = false;
};

// Returns false if the program should stop: either --help, or an argument that
// could not be understood. Unknown arguments are rejected rather than ignored.
bool parseArguments(int argc, char** argv, Arguments& args, int& exitCode)
{
    for (int i = 1; i < argc; ++i) {
        const string arg = argv[i];

        if (arg == "--help" || arg == "-h") {
            printUsage();
            exitCode = 0;
            return false;
        }
        if (arg == "--histogram") {
            args.histogram = true;
            continue;
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
            else if (arg == "--ring-buffer-mb") args.ringBufferMb = stoi(value);
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
    // means the two cannot disagree.
    settings.rig = args.rig.empty() ? ("cam_" + args.serial_number) : args.rig;

    if (!args.mode.empty())   settings.recording_mode = args.mode;
    if (args.fps)             settings.fps = *args.fps;
    if (args.exposureMin)     settings.exposure_lower_limit_us = *args.exposureMin;
    if (args.streamBuffers)   settings.stream_buffers = *args.streamBuffers;
    if (args.strobeLine)      settings.strobe_line = *args.strobeLine;
    if (args.qp)              settings.video.qp = *args.qp;
    if (args.gop)             settings.video.gop = *args.gop;
    if (args.ringBufferMb)    settings.ring_buffer_mb = *args.ringBufferMb;
    if (args.windowWidth)     settings.window_width = *args.windowWidth;
    if (args.windowHeight)    settings.window_height = *args.windowHeight;
    settings.show_histogram = args.histogram;

    string path = args.path;
    if (path.empty()) {
        path = (fs::path(settings.output_root) / (args.date_time + "_" + args.mouse_ID)).string();
    }

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

    // From here on, everything printed also lands in the session folder, which is
    // the only record of what happened on a rig launched without a console.
    SessionLog log(fs::path(path) / (args.date_time + "_" + args.mouse_ID + "_camera_log.txt"));

    try {
        CameraRecorder camera(args.mouse_ID, args.date_time, path, args.serial_number, settings);
        const bool completed = camera.startRecording(args.showPreview, true);
        return completed ? 0 : 1;
    }
    catch (const std::exception& e) {
        cerr << "Error: " << e.what() << endl;
        return 2;
    }
}
