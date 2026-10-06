// camera_probe - read-only camera diagnostic.
//
// Enumerates the attached Spinnaker cameras, prints what each one reports about
// itself, and optionally grabs a burst of frames to measure the rate actually
// achieved. Nothing is written to disk, so a result here reflects the camera and
// its link alone: if frames are lost during a probe the cause is upstream of the
// recorder, and if they are not, it is not.
//
//   camera_probe                       list cameras and exit
//   camera_probe --serial 26043809     describe that camera in detail
//   camera_probe --serial 26043809 --frames 600 --fps 60
//
// Exit codes: 0 = clean, 1 = ran but lost frames, 2 = error.

#include <Spinnaker.h>
#include <SpinGenApi/SpinnakerGenApi.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

using namespace Spinnaker;
using namespace Spinnaker::GenApi;
using Clock = std::chrono::steady_clock;

namespace {

// ---------------------------------------------------------------------------
// Node reading. Every read is guarded: which nodes exist varies by model and by
// whether the camera has been initialised, so an unreadable node is normal
// rather than an error.
// ---------------------------------------------------------------------------

std::string readString(INodeMap& nodeMap, const char* name)
{
    CStringPtr node = nodeMap.GetNode(name);
    if (IsReadable(node)) {
        return std::string(node->GetValue().c_str());
    }
    return {};
}

std::string readEnum(INodeMap& nodeMap, const char* name)
{
    CEnumerationPtr node = nodeMap.GetNode(name);
    if (IsReadable(node)) {
        CEnumEntryPtr entry = node->GetCurrentEntry();
        if (IsReadable(entry)) {
            return std::string(entry->GetSymbolic().c_str());
        }
    }
    return {};
}

bool readInt(INodeMap& nodeMap, const char* name, int64_t& out)
{
    CIntegerPtr node = nodeMap.GetNode(name);
    if (!IsReadable(node)) {
        return false;
    }
    out = node->GetValue();
    return true;
}

bool readFloat(INodeMap& nodeMap, const char* name, double& out)
{
    CFloatPtr node = nodeMap.GetNode(name);
    if (!IsReadable(node)) {
        return false;
    }
    out = node->GetValue();
    return true;
}

bool readFloatRange(INodeMap& nodeMap, const char* name, double& value, double& lo, double& hi)
{
    CFloatPtr node = nodeMap.GetNode(name);
    if (!IsReadable(node)) {
        return false;
    }
    value = node->GetValue();
    lo = node->GetMin();
    hi = node->GetMax();
    return true;
}

void printField(const char* label, const std::string& value)
{
    if (!value.empty()) {
        std::cout << "  " << std::left << std::setw(26) << label << value << "\n";
    }
}

// ---------------------------------------------------------------------------
// Description
// ---------------------------------------------------------------------------

// Summary usable before the camera is initialised, from the transport layer.
std::string summarise(const CameraPtr& pCam)
{
    INodeMap& tl = pCam->GetTLDeviceNodeMap();
    const std::string serial = readString(tl, "DeviceSerialNumber");
    const std::string model = readString(tl, "DeviceModelName");
    const std::string speed = readEnum(tl, "DeviceCurrentSpeed");

    std::string out = "serial " + (serial.empty() ? std::string("<unknown>") : serial);
    if (!model.empty()) {
        out += ", " + model;
    }
    if (!speed.empty()) {
        out += ", link " + speed;
    }
    return out;
}

// Fuller description, requiring an initialised camera.
void describe(const CameraPtr& pCam)
{
    INodeMap& tl = pCam->GetTLDeviceNodeMap();
    std::cout << "\nDevice\n";
    printField("serial", readString(tl, "DeviceSerialNumber"));
    printField("vendor", readString(tl, "DeviceVendorName"));
    printField("model", readString(tl, "DeviceModelName"));
    printField("type", readEnum(tl, "DeviceType"));
    printField("link speed", readEnum(tl, "DeviceCurrentSpeed"));

    INodeMap& nodeMap = pCam->GetNodeMap();

    std::cout << "\nImage\n";
    int64_t width = 0;
    int64_t height = 0;
    if (readInt(nodeMap, "Width", width) && readInt(nodeMap, "Height", height)) {
        printField("resolution", std::to_string(width) + " x " + std::to_string(height));
    }
    printField("pixel format", readEnum(nodeMap, "PixelFormat"));

    // Bytes per frame drives every bandwidth and disk-space question, so state it
    // rather than leaving it to be worked out from the resolution.
    int64_t frameBytes = 0;
    if (readInt(nodeMap, "PayloadSize", frameBytes)) {
        std::cout << "  " << std::left << std::setw(26) << "frame size"
                  << frameBytes << " bytes (" << std::fixed << std::setprecision(2)
                  << frameBytes / 1048576.0 << " MiB)\n";
    }

    std::cout << "\nAcquisition\n";
    double fps = 0.0;
    double fpsLo = 0.0;
    double fpsHi = 0.0;
    if (readFloatRange(nodeMap, "AcquisitionFrameRate", fps, fpsLo, fpsHi)) {
        std::cout << "  " << std::left << std::setw(26) << "frame rate"
                  << std::fixed << std::setprecision(2) << fps
                  << " fps (settable " << fpsLo << " to " << fpsHi << ")\n";
        if (frameBytes > 0) {
            std::cout << "  " << std::left << std::setw(26) << "data rate at this rate"
                      << std::fixed << std::setprecision(1)
                      << (frameBytes * fps) / 1e6 << " MB/s, "
                      << (frameBytes * fps * 3600.0) / 1e9 << " GB/hour\n";
        }
    }
    printField("exposure auto", readEnum(nodeMap, "ExposureAuto"));
    double exposure = 0.0;
    if (readFloat(nodeMap, "ExposureTime", exposure)) {
        std::cout << "  " << std::left << std::setw(26) << "exposure now"
                  << std::fixed << std::setprecision(0) << exposure << " us\n";
    }
    double temperature = 0.0;
    if (readFloat(nodeMap, "DeviceTemperature", temperature)) {
        std::cout << "  " << std::left << std::setw(26) << "temperature"
                  << std::fixed << std::setprecision(1) << temperature << " C\n";
    }

    // Worth seeing explicitly: set below the rate the camera needs, the throughput
    // limit throttles it silently and the result looks like dropped frames.
    std::cout << "\nLink\n";
    int64_t limit = 0;
    if (readInt(nodeMap, "DeviceLinkThroughputLimit", limit)) {
        std::cout << "  " << std::left << std::setw(26) << "throughput limit"
                  << std::fixed << std::setprecision(1) << limit / 1e6
                  << " MB/s (" << limit << " B/s)\n";
    }
    int64_t currentThroughput = 0;
    if (readInt(nodeMap, "DeviceLinkCurrentThroughput", currentThroughput)) {
        std::cout << "  " << std::left << std::setw(26) << "current throughput"
                  << std::fixed << std::setprecision(1) << currentThroughput / 1e6 << " MB/s\n";
    }

    std::cout << "\nStream buffers\n";
    INodeMap& stream = pCam->GetTLStreamNodeMap();
    printField("buffer count mode", readEnum(stream, "StreamBufferCountMode"));
    int64_t buffers = 0;
    if (readInt(stream, "StreamBufferCountResult", buffers)) {
        printField("buffers in use", std::to_string(buffers));
    }
    printField("handling mode", readEnum(stream, "StreamBufferHandlingMode"));
}

// ---------------------------------------------------------------------------
// Grab test
// ---------------------------------------------------------------------------

struct GrabResult
{
    int64_t requested = 0;
    int64_t received = 0;
    int64_t incomplete = 0;
    int64_t failures = 0;
    int64_t droppedFromIds = 0;   // gaps in the camera's own frame counter
    double elapsedSeconds = 0.0;
    std::vector<double> intervalsMs;
};

// Requests a frame rate, clamped to what the camera currently permits. The
// achievable maximum depends on exposure, ROI and pixel format, so it is read
// from the device rather than held in code.
bool setFrameRate(INodeMap& nodeMap, double target, std::string& note)
{
    CBooleanPtr enable = nodeMap.GetNode("AcquisitionFrameRateEnable");
    if (IsWritable(enable)) {
        enable->SetValue(true);
    }

    CFloatPtr rate = nodeMap.GetNode("AcquisitionFrameRate");
    if (!IsWritable(rate)) {
        note = "AcquisitionFrameRate is not writable";
        return false;
    }

    const double lo = rate->GetMin();
    const double hi = rate->GetMax();
    const double clamped = std::clamp(target, lo, hi);
    if (clamped != target) {
        std::ostringstream message;
        message << std::fixed << std::setprecision(2)
                << "requested " << target << " fps, clamped to " << clamped
                << " (camera allows " << lo << " to " << hi << ")";
        note = message.str();
    }
    rate->SetValue(clamped);
    return true;
}

GrabResult grabTest(const CameraPtr& pCam, int64_t frames, double nominalFps)
{
    GrabResult result;
    result.requested = frames;

    // Long enough that a slow camera is not mistaken for a stalled one, short
    // enough that a dead link does not hang the probe.
    const uint64_t timeoutMs = nominalFps > 0.0
        ? static_cast<uint64_t>(std::max(1000.0, 10000.0 / nominalFps))
        : 2000u;

    pCam->BeginAcquisition();

    int64_t previousId = -1;
    bool haveArrival = false;
    Clock::time_point previousArrival{};
    const Clock::time_point start = Clock::now();

    for (int64_t i = 0; i < frames; ++i) {
        try {
            ImagePtr image = pCam->GetNextImage(timeoutMs);
            const Clock::time_point arrival = Clock::now();

            if (image->IsIncomplete()) {
                ++result.incomplete;
                std::cout << "  incomplete frame: "
                          << Image::GetImageStatusDescription(image->GetImageStatus()) << "\n";
            } else {
                ++result.received;

                const int64_t id = static_cast<int64_t>(image->GetFrameID());
                if (previousId >= 0 && id > previousId + 1) {
                    result.droppedFromIds += id - previousId - 1;
                }
                previousId = id;

                if (haveArrival) {
                    result.intervalsMs.push_back(
                        std::chrono::duration<double, std::milli>(arrival - previousArrival).count());
                }
                previousArrival = arrival;
                haveArrival = true;
            }

            image->Release();
        }
        catch (Spinnaker::Exception& e) {
            ++result.failures;
            std::cout << "  grab failed: " << e.what() << "\n";
            if (result.failures > 10) {
                std::cout << "  too many failures, stopping early\n";
                break;
            }
        }
    }

    result.elapsedSeconds = std::chrono::duration<double>(Clock::now() - start).count();
    pCam->EndAcquisition();
    return result;
}

void reportGrab(const GrabResult& result, double nominalFps, const CameraPtr& pCam)
{
    std::cout << "\nResult\n";
    std::cout << "  " << std::left << std::setw(26) << "frames requested" << result.requested << "\n";
    std::cout << "  " << std::left << std::setw(26) << "frames received" << result.received << "\n";
    std::cout << "  " << std::left << std::setw(26) << "incomplete" << result.incomplete << "\n";
    std::cout << "  " << std::left << std::setw(26) << "failed grabs" << result.failures << "\n";
    std::cout << "  " << std::left << std::setw(26) << "dropped (frame-ID gaps)"
              << result.droppedFromIds << "\n";

    if (result.elapsedSeconds > 0.0) {
        std::cout << "  " << std::left << std::setw(26) << "measured rate"
                  << std::fixed << std::setprecision(2)
                  << result.received / result.elapsedSeconds << " fps";
        if (nominalFps > 0.0) {
            std::cout << " (asked for " << nominalFps << ")";
        }
        std::cout << "\n";
    }

    // The spread says more than the mean: a camera that mostly keeps up but
    // stalls occasionally has a healthy average and a long tail.
    if (!result.intervalsMs.empty()) {
        std::vector<double> sorted = result.intervalsMs;
        std::sort(sorted.begin(), sorted.end());
        const double median = sorted[sorted.size() / 2];
        const double p99 = sorted[std::min(sorted.size() - 1,
                                           static_cast<size_t>(sorted.size() * 0.99))];
        std::cout << "  " << std::left << std::setw(26) << "frame interval ms"
                  << std::fixed << std::setprecision(2)
                  << "min " << sorted.front()
                  << ", median " << median
                  << ", p99 " << p99
                  << ", max " << sorted.back() << "\n";
    }

    // The driver's own accounting, which can catch losses the frame IDs do not.
    try {
        INodeMap& stream = pCam->GetTLStreamNodeMap();
        int64_t value = 0;
        if (readInt(stream, "StreamDroppedFrameCount", value)) {
            std::cout << "  " << std::left << std::setw(26) << "driver dropped count" << value << "\n";
        }
        if (readInt(stream, "StreamFailedBufferCount", value)) {
            std::cout << "  " << std::left << std::setw(26) << "failed buffers" << value << "\n";
        }
    }
    catch (Spinnaker::Exception&) {
        // Stream statistics are optional; their absence is not a failure.
    }

    const bool clean = result.incomplete == 0 && result.failures == 0 && result.droppedFromIds == 0;
    if (clean) {
        std::cout << "\n  Clean: every frame the camera produced arrived intact.\n";
    } else {
        std::cout << "\n  Lost frames during the probe. Nothing was written to disk here, so\n"
                     "  the cause is the camera, its cable, or the USB link, not the recorder.\n";
    }
}

// ---------------------------------------------------------------------------
// Command line
// ---------------------------------------------------------------------------

struct Options
{
    std::string serial;
    int64_t frames = 0;
    double fps = 0.0;
    bool listOnly = true;
};

bool parseArgs(int argc, char** argv, Options& options)
{
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        const bool hasValue = (i + 1 < argc);

        if (arg == "--serial" && hasValue) {
            options.serial = argv[++i];
            options.listOnly = false;
        } else if (arg == "--frames" && hasValue) {
            options.frames = std::strtoll(argv[++i], nullptr, 10);
            options.listOnly = false;
        } else if (arg == "--fps" && hasValue) {
            options.fps = std::strtod(argv[++i], nullptr);
            options.listOnly = false;
        } else if (arg == "--help" || arg == "-h") {
            std::cout <<
                "camera_probe - read-only camera diagnostic\n"
                "\n"
                "  --serial <id>   camera to examine (default: the only one attached)\n"
                "  --frames <n>    grab n frames and measure the rate achieved\n"
                "  --fps <rate>    frame rate to request before grabbing\n"
                "  --help          this message\n"
                "\n"
                "With no arguments, lists the cameras it can see and exits.\n";
            return false;
        } else {
            std::cerr << "Unrecognised argument: " << arg << "\n"
                      << "Run with --help to see the accepted options.\n";
            return false;
        }
    }
    return true;
}

} // namespace

int main(int argc, char** argv)
{
    Options options;
    if (!parseArgs(argc, argv, options)) {
        return 2;
    }

    SystemPtr system = System::GetInstance();
    int exitCode = 0;

    try {
        const LibraryVersion version = system->GetLibraryVersion();
        std::cout << "Spinnaker " << version.major << "." << version.minor
                  << "." << version.type << "." << version.build << "\n";

        CameraList cameras = system->GetCameras();
        const unsigned int count = cameras.GetSize();
        std::cout << count << " camera" << (count == 1 ? "" : "s") << " detected\n";

        for (unsigned int i = 0; i < count; ++i) {
            std::cout << "  [" << i << "] " << summarise(cameras.GetByIndex(i)) << "\n";
        }

        if (count == 0) {
            std::cerr << "\nNo cameras found. Check the USB connection, and that nothing else\n"
                         "(SpinView, another recording) already has the camera open.\n";
            cameras.Clear();
            system->ReleaseInstance();
            return 2;
        }

        if (!options.listOnly) {
            // Pick the requested camera, or the only one if no serial was given.
            CameraPtr pCam = nullptr;
            if (!options.serial.empty()) {
                pCam = cameras.GetBySerial(options.serial);
                if (!pCam) {
                    std::cerr << "\nNo camera with serial " << options.serial
                              << ". The serials detected are listed above.\n";
                    cameras.Clear();
                    system->ReleaseInstance();
                    return 2;
                }
            } else if (count == 1) {
                pCam = cameras.GetByIndex(0);
            } else {
                std::cerr << "\nSeveral cameras attached; name one with --serial.\n";
                cameras.Clear();
                system->ReleaseInstance();
                return 2;
            }

            pCam->Init();

            if (options.fps > 0.0) {
                std::string note;
                if (setFrameRate(pCam->GetNodeMap(), options.fps, note)) {
                    if (!note.empty()) {
                        std::cout << "\nNote: " << note << "\n";
                    }
                } else {
                    std::cout << "\nCould not set the frame rate: " << note << "\n";
                }
            }

            describe(pCam);

            if (options.frames > 0) {
                double nominal = 0.0;
                readFloat(pCam->GetNodeMap(), "AcquisitionFrameRate", nominal);

                std::cout << "\nGrabbing " << options.frames << " frames";
                if (nominal > 0.0) {
                    std::cout << " (about " << std::fixed << std::setprecision(1)
                              << options.frames / nominal << " s)";
                }
                std::cout << "\n";

                const GrabResult result = grabTest(pCam, options.frames, nominal);
                reportGrab(result, nominal, pCam);
                if (result.incomplete != 0 || result.failures != 0 || result.droppedFromIds != 0) {
                    exitCode = 1;
                }
            }

            pCam->DeInit();
            pCam = nullptr;
        }

        cameras.Clear();
    }
    catch (Spinnaker::Exception& e) {
        std::cerr << "\nSpinnaker error: " << e.what() << "\n";
        exitCode = 2;
    }
    catch (std::exception& e) {
        std::cerr << "\nError: " << e.what() << "\n";
        exitCode = 2;
    }

    // Every camera reference must be gone before the system is released.
    system->ReleaseInstance();
    return exitCode;
}
