// Standard library includes
#include <iostream>
#include <chrono>
#include <sstream>
#include <fstream>
#include <vector>
#include <string>
#include <cstdlib>
#include <filesystem>

// Platform-specific includes
#include <direct.h>

// Third-party library includes
#include <GLFW/glfw3.h>  // Must be included before any OpenGL headers
#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>
#include "nlohmann/json.hpp"

// Spinnaker SDK includes
#include "Spinnaker.h"
#include "SpinGenApi/SpinnakerGenApi.h"

using namespace Spinnaker;
using namespace Spinnaker::GenApi;
using namespace Spinnaker::GenICam;
using namespace std;
using namespace std::chrono;
using json = nlohmann::json;
namespace fs = std::filesystem;

GLFWwindow* initializeOpenGL(int width, int height, const std::string& title)
{
    // Initialize GLFW
    if (!glfwInit())
    {
        std::cerr << "Failed to initialize GLFW" << std::endl;
        exit(EXIT_FAILURE);
    }

    // Create a GLFW window
    GLFWwindow* window = glfwCreateWindow(width, height, title.c_str(), NULL, NULL);
    if (!window)
    {
        std::cerr << "Failed to create GLFW window" << std::endl;
        glfwTerminate();
        exit(EXIT_FAILURE);
    }

    glfwMakeContextCurrent(window);
    glViewport(0, 0, width, height);
    return window;
}

void causeSpinnakerException() {
    throw Spinnaker::Exception(
        __LINE__,                          // Current line number
        __FILE__,                          // Current source file
        __FUNCTION__,                      // Current function name
        "Simulated resource conflict",     // Error message
        Spinnaker::SPINNAKER_ERR_RESOURCE_IN_USE  // Error code
    );
}

class CameraRecorder
{
public:
    // Constructor
    CameraRecorder(const string& mouse_ID, const string& start_time, const string& path,
        const string& serial_number, float FPS, int windowWidth, int windowHeight)
        : mouse_ID(mouse_ID), start_time(start_time), path(path),
        camSerial(serial_number), FPS(FPS), windowWidth(windowWidth),
        windowHeight(windowHeight), frame_count(0)
    {
        system = System::GetInstance();
        CameraList camList = system->GetCameras();

        // Which rig each camera sits on. This is only a naming table: it decides the
        // window title and the signal-file names, nothing about how the camera is
        // driven. An unlisted serial is therefore not an error — it records exactly
        // the same, it just has no friendly name — so it gets one from its serial
        // instead of being refused. Previously a new camera meant editing this list
        // and redeploying to every rig machine before it could be used at all.
        static const struct { const char* serial; const char* rig; } knownCameras[] = {
            { "22181614", "1" },
            { "20530175", "2" },
            { "24174008", "3" },
            { "24243513", "4" },
            { "24174020", "openfield" },
            { "23606054", "colour_camera" },
            { "21423798", "6MP3_camera" },
        };

        for (const auto& known : knownCameras) {
            if (camSerial == known.serial) {
                rig = known.rig;
                break;
            }
        }
        if (rig.empty()) {
            rig = "cam_" + camSerial;
            cout << "Camera " << camSerial << " is not in the known-rig list; "
                 << "calling it '" << rig << "'." << endl;
        }

        // Note: the frame rate is not clamped here. The rate a camera can sustain
        // depends on its pixel format, ROI and exposure, so it is read from the
        // device once it is open, in setCameraFrameRate below.

        windowTitle << "Rig " << rig << ". Press 'Esc' to stop session.";
        title = windowTitle.str();

        // Use GetBySerial to get the camera
        pCam = camList.GetBySerial(camSerial);

        if (!pCam) {
            cerr << "Error: Camera can't open\nexit" << endl;
            throw runtime_error("Camera can't open");
        }

        pCam->Init();

        // Assigning the result back to this->FPS matters. The constructor parameter
        // is also called FPS and shadows the member, so the member keeps whatever
        // was asked for unless it is set explicitly here — and the member is what
        // gets written into the metadata JSON and reused by attemptRecovery(). Those
        // used to disagree whenever a rate above the camera's maximum was requested.
        this->FPS = static_cast<float>(setCameraFrameRate(FPS));
        setGPIOLine2ToOutput();     // Set GPIO Line 2 to output
        setExposureTimeLowerLimit(4000.0);  // Set exposure time lower limit

        INodeMap& nodeMap = pCam->GetNodeMap();

        // Set acquisition mode to continuous
        CEnumerationPtr ptrAcquisitionMode = nodeMap.GetNode("AcquisitionMode");
        if (!IsReadable(ptrAcquisitionMode) || !IsWritable(ptrAcquisitionMode)) {
            cerr << "Error: Unable to set acquisition mode to continuous." << endl;
            throw runtime_error("Unable to set acquisition mode to continuous");
        }

        CEnumEntryPtr ptrAcquisitionModeContinuous =
            ptrAcquisitionMode->GetEntryByName("Continuous");
        if (!IsReadable(ptrAcquisitionModeContinuous)) {
            cerr << "Error: Unable to get or set acquisition mode to continuous." << endl;
            throw runtime_error("Unable to set acquisition mode to continuous");
        }

        const int64_t acquisitionModeContinuous = ptrAcquisitionModeContinuous->GetValue();
        ptrAcquisitionMode->SetIntValue(acquisitionModeContinuous);

        pCam->BeginAcquisition();
        acquiring = true;

        imageWidth = pCam->Width.GetValue();
        imageHeight = pCam->Height.GetValue();
        pixelFormat = pCam->PixelFormat.GetCurrentEntry()->GetSymbolic();

        // Open the binary file for writing
        stringstream binFilename;
        binFilename << path << "/" + start_time + "_" + mouse_ID + "_binary_video.bin";
        imageFile.open(binFilename.str(), ios::binary | ios::out);
        if (!imageFile.is_open()) {
            cerr << "Error: Could not open binary file for writing." << endl;
            throw runtime_error("Could not open binary file for writing");
        }
    }

    // Destructor
    //
    // Every Spinnaker call here can throw, and a destructor is implicitly noexcept:
    // an escaping exception calls std::terminate, which showed up as the process
    // dying with 0xC0000409 at the end of every otherwise successful session. The
    // data was always safe — it is written and the finished-signal file created
    // before this runs — but the exit code said the run had failed, and a launcher
    // checking it could not tell a real failure from a normal finish.
    //
    // Two things fix it: only end acquisition if it is actually running (captureFrames
    // has usually ended it already, and ending it twice is what threw), and let
    // nothing escape.
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

        // Released whether or not the above succeeded: the system instance must
        // outlive every camera reference, so this has to happen after pCam is gone.
        pCam = nullptr;

        if (imageFile.is_open()) {
            imageFile.close();
        }

        try {
            if (system) {
                system->ReleaseInstance();
            }
        }
        catch (Spinnaker::Exception& e) {
            cerr << "Warning: error releasing the Spinnaker system: " << e.what() << endl;
        }
    }

    void startRecording(bool show_frame, bool save_video)
    {
        frame_IDs.clear();
        timer_start_time = high_resolution_clock::now();
        saveData();

        // Start the capture loop
        captureFrames(show_frame, save_video);

        end_time = currentDateTime();
        saveData();

        // Create the signal file to indicate that tracking has finished
        createSignalFile();
    }

private:
    string mouse_ID;
    string start_time;
    string end_time;
    string path;
    string camSerial;
    string rig;
    float FPS;
    size_t frame_count;
    // Whether BeginAcquisition is currently in effect. Ending acquisition twice
    // throws, and the second call used to come from the destructor.
    bool acquiring = false;
    CameraPtr pCam;
    SystemPtr system;
    vector<uint64_t> frame_IDs;
    vector<uint64_t> frame_IDs_mem;
    high_resolution_clock::time_point timer_start_time;
    ostringstream windowTitle;
    string title;
    int windowWidth;
    int windowHeight;
    size_t imageWidth;
    size_t imageHeight;
    string pixelFormat;
    ofstream imageFile;  // Binary file to store image data
    const int SIGNAL_CHECK_INTERVAL = 30;  // Check for signal every 30 frames

    const size_t bufferSize = 200;

    int recoveryAttempts = 0;
    const int MAX_RECOVERY_ATTEMPTS = 3;
    const std::chrono::seconds RECOVERY_COOLDOWN{ 5 };

    const size_t FRAMES_BEFORE_TEST_ERROR = 300; // Will trigger error after ~3 seconds at 60 FPS
    size_t test_error_counter = 0;
    bool test_error_triggered = false;



    void captureFrames(bool show_frame, bool save_video) {
        auto prev = high_resolution_clock::now();
        int displayFPS = 30;  // Maximum display FPS
        int frame_skip = int(1000 / displayFPS);  // Frame skip duration in ms

        // Open the frame ID file in append mode
        ofstream frameIDFile(path + "/" + start_time + "_" + mouse_ID + "_frame_ids_backup.txt", ios_base::app);
        if (!frameIDFile.is_open()) {
            cerr << "Error: Could not open frame ID file for writing." << endl;
            return;
        }

        bool keepRunning = true;

        // OpenGL: Initialize GLFW for OpenGL window management
        GLFWwindow* window = nullptr;
        if (show_frame) {
            if (!glfwInit()) {
                cerr << "Error: Failed to initialize GLFW" << endl;
                return;
            }

            // Create a GLFW window
            window = glfwCreateWindow(windowWidth, windowHeight, title.c_str(), NULL, NULL);
            if (!window) {
                cerr << "Error: Failed to create GLFW window" << endl;
                glfwTerminate();
                return;
            }

            glfwMakeContextCurrent(window);
            glViewport(0, 0, windowWidth, windowHeight);

            // Set OpenGL clear color (background)
            glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        }

        while (keepRunning) {
            try {

                ImagePtr pResultImage = pCam->GetNextImage(1000);

                if (!pResultImage || pResultImage->IsIncomplete()) {
                    if (pResultImage) pResultImage->Release();

                    cerr << "Error: Image incomplete or null" << endl;

                    if (!attemptRecovery()) {
                        cerr << "Unable to recover camera. Stopping recording." << endl;
                        keepRunning = false;
                        break;
                    }

                    std::this_thread::sleep_for(RECOVERY_COOLDOWN);
                    continue;
                }

                // Reset recovery attempts on successful frame
                recoveryAttempts = 0;

                if (save_video) {
                    // Write raw image data to the binary file
                    const char* imageData = reinterpret_cast<const char*>(pResultImage->GetData());
                    size_t imageSize = pResultImage->GetImageSize();

                    imageFile.write(imageData, imageSize);
                    if (!imageFile.good()) {
                        cerr << "Error: Failed to write image data to binary file." << endl;
                        pResultImage->Release();
                        keepRunning = false;
                        break;
                    }

                    // Add frame ID to the list
                    uint64_t frameID = pResultImage->GetFrameID();
                    frame_IDs.push_back(frameID);       // Save to frame_IDs
                    frame_IDs_mem.push_back(frameID);   // Save to frame_IDs_mem

                    // Flush frame IDs to file if buffer is full
                    if (frame_IDs.size() >= bufferSize) {
                        for (const auto& id : frame_IDs) {
                            frameIDFile << id << std::endl;
                        }
                        frameIDFile.flush();
                        frame_IDs.clear();
                    }
                }

                // Display frames at the specified display FPS
                if (show_frame) {
                    auto now = high_resolution_clock::now();
                    double elapsedTime = duration_cast<milliseconds>(now - prev).count();

                    if (elapsedTime >= frame_skip) {
                        // Convert image to OpenGL texture format
                        cv::Mat image(cv::Size(imageWidth, imageHeight), CV_8UC1,
                            pResultImage->GetData(), pResultImage->GetStride());

                        // Resize the image to fit the OpenGL window
                        cv::Mat resizedImage;
                        cv::resize(image, resizedImage, cv::Size(windowWidth, windowHeight));

                        // Clear the OpenGL buffer
                        glClear(GL_COLOR_BUFFER_BIT);

                        // Use glDrawPixels to display the image
                        glPixelZoom(1.0f, -1.0f);  // Flip the image vertically
                        glRasterPos2i(-1, 1);      // Set image position
                        glDrawPixels(resizedImage.cols, resizedImage.rows, GL_LUMINANCE, GL_UNSIGNED_BYTE, resizedImage.data);

                        // Swap buffers to display the image
                        glfwSwapBuffers(window);

                        // Poll for input events
                        glfwPollEvents();

                        // check for signal file from startup program
                        if (checkForStopSignal()) {
                            keepRunning = false;
                        }

                        // Check if the user pressed the 'Esc' key or closed the window
                        if (glfwGetKey(window, GLFW_KEY_ESCAPE) == GLFW_PRESS || glfwWindowShouldClose(window)) {
                            keepRunning = false;
                        }

                        prev = now;  // Reset the previous time for the next displayed frame
                    }
                }

                pResultImage->Release();
                frame_count++;

            }
            catch (Spinnaker::Exception& e) {
                cerr << "Camera error: " << e.what() << endl;

                if (!attemptRecovery()) {
                    cerr << "Unable to recover from error. Stopping recording." << endl;
                    keepRunning = false;
                    break;
                }

                std::this_thread::sleep_for(RECOVERY_COOLDOWN);
            }
        }

        if (pCam && acquiring) {
            pCam->EndAcquisition();
            acquiring = false;
        }

        // After the loop, flush any remaining frame IDs in the buffer
        if (!frame_IDs.empty()) {
            for (const auto& frameID : frame_IDs) {
                frameIDFile << frameID << std::endl;
            }
            frameIDFile.flush();
            frame_IDs.clear();
        }

        // Cleanup OpenGL resources
        if (window) {
            glfwDestroyWindow(window);
            glfwTerminate();
        }

        frameIDFile.close();
    }

    bool attemptRecovery() {
        if (recoveryAttempts >= MAX_RECOVERY_ATTEMPTS) {
            cerr << "Max recovery attempts reached. Camera error persists." << endl;
            return false;
        }

        try {
            cerr << "Attempting camera recovery (attempt " << recoveryAttempts + 1 << " of " << MAX_RECOVERY_ATTEMPTS << ")..." << endl;

            if (acquiring) {
                pCam->EndAcquisition();
                acquiring = false;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(500));

            // Reset camera settings
            pCam->DeInit();
            std::this_thread::sleep_for(std::chrono::milliseconds(500));

            pCam->Init();
            setCameraFrameRate(FPS);
            setGPIOLine2ToOutput();
            setExposureTimeLowerLimit(4000.0);

            pCam->BeginAcquisition();
            acquiring = true;

            // Test if camera is working
            ImagePtr testImage = pCam->GetNextImage(1000);
            if (testImage && !testImage->IsIncomplete()) {
                testImage->Release();
                cerr << "Camera recovered successfully" << endl;
                recoveryAttempts = 0;  // Reset counter on successful recovery
                return true;
            }
            testImage->Release();

            recoveryAttempts++;
            return false;

        }
        catch (Spinnaker::Exception& e) {
            cerr << "Recovery attempt failed: " << e.what() << endl;
            recoveryAttempts++;
            return false;
        }
    }

    bool checkForStopSignal() {
        if (frame_count % SIGNAL_CHECK_INTERVAL != 0) {
            return false;  // Only check every Nth frame
        }

        string stop_signal_path = fs::path(path).string() + "/stop_camera_" + rig + ".signal";
        return fs::exists(stop_signal_path);
    }

    GLFWwindow* setupOpenGLWindow() {
        if (!glfwInit()) {
            cerr << "Error: Failed to initialize GLFW" << endl;
            return nullptr;
        }

        GLFWwindow* window = glfwCreateWindow(windowWidth, windowHeight, title.c_str(), NULL, NULL);
        if (!window) {
            cerr << "Error: Failed to create GLFW window" << endl;
            glfwTerminate();
            return nullptr;
        }

        glfwMakeContextCurrent(window);
        glViewport(0, 0, windowWidth, windowHeight);
        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);

        return window;
    }

    bool saveFrame(ImagePtr& pResultImage, ofstream& frameIDFile) {
        const char* imageData = reinterpret_cast<const char*>(pResultImage->GetData());
        size_t imageSize = pResultImage->GetImageSize();

        imageFile.write(imageData, imageSize);
        if (!imageFile.good()) {
            return false;
        }

        uint64_t frameID = pResultImage->GetFrameID();
        frame_IDs.push_back(frameID);
        frame_IDs_mem.push_back(frameID);

        if (frame_IDs.size() >= bufferSize) {
            for (const auto& id : frame_IDs) {
                frameIDFile << id << std::endl;
            }
            frameIDFile.flush();
            frame_IDs.clear();
        }

        return true;
    }

    void cleanupCapture(ofstream& frameIDFile, GLFWwindow* window) {
        if (!frame_IDs.empty()) {
            for (const auto& frameID : frame_IDs) {
                frameIDFile << frameID << std::endl;
            }
            frameIDFile.flush();
            frame_IDs.clear();
        }

        frameIDFile.close();

        if (window) {
            glfwDestroyWindow(window);
            glfwTerminate();
        }
    }

    void saveData()
    {
        string file_name = start_time + "_" + mouse_ID + "_Tracker_data.json";
        json data;

        data["frame_rate"] = FPS;
        data["start_time"] = start_time;
        data["end_time"] = end_time;
        data["image_height"] = imageHeight;
        data["image_width"] = imageWidth;
        data["pixel_format"] = pixelFormat;
        data["frame_IDs"] = frame_IDs_mem;

        ofstream file(path + "/" + file_name);
        file << data.dump(4);  // Pretty print with 4 spaces
        file.close();
    }

    string currentDateTime()
    {
        auto now = system_clock::now();
        time_t now_time = system_clock::to_time_t(now);
        char buffer[80];
        tm localTime;
        localtime_s(&localTime, &now_time);
        strftime(buffer, sizeof(buffer), "%y%m%d_%H%M%S", &localTime);
        return string(buffer);
    }

    // Sets the acquisition frame rate, clamped to what this camera currently
    // permits, and returns the rate actually applied so the caller can record the
    // truth rather than the request. The permitted range depends on pixel format,
    // ROI and exposure time, which is why it is read from the device here instead
    // of being kept in a table that has to be maintained by hand.
    double setCameraFrameRate(double frameRate)
    {
        INodeMap& nodeMap = pCam->GetNodeMap();
        CBooleanPtr ptrFrameRateEnable = nodeMap.GetNode("AcquisitionFrameRateEnable");
        if (IsWritable(ptrFrameRateEnable)) {
            ptrFrameRateEnable->SetValue(true);
        }
        else {
            throw runtime_error("Unable to enable frame rate");
        }

        CFloatPtr ptrFrameRate = nodeMap.GetNode("AcquisitionFrameRate");
        if (!IsWritable(ptrFrameRate)) {
            throw runtime_error("Unable to set frame rate");
        }

        const double minRate = ptrFrameRate->GetMin();
        const double maxRate = ptrFrameRate->GetMax();
        double applied = frameRate;
        if (applied < minRate) {
            applied = minRate;
        }
        else if (applied > maxRate) {
            applied = maxRate;
        }

        if (applied != frameRate) {
            cout << "Requested " << frameRate << " fps; this camera allows "
                 << minRate << " to " << maxRate << " fps, so recording at "
                 << applied << " fps." << endl;
        }

        ptrFrameRate->SetValue(applied);

        // Read it back rather than returning what we asked for. The camera
        // quantises the rate to what its timing can actually produce, so a request
        // for 60 may become 59.99, and the metadata should record the rate the
        // frames were really captured at.
        return ptrFrameRate->GetValue();
    }

    void setGPIOLine2ToOutput()
    {
        INodeMap& nodeMap = pCam->GetNodeMap();

        // Select Line 2
        CEnumerationPtr ptrLineSelector = nodeMap.GetNode("LineSelector");
        if (IsWritable(ptrLineSelector)) {
            CEnumEntryPtr ptrLine2 = ptrLineSelector->GetEntryByName("Line2");
            if (IsReadable(ptrLine2)) {
                ptrLineSelector->SetIntValue(ptrLine2->GetValue());
            }
            else {
                throw runtime_error("Unable to select Line 2");
            }
        }
        else {
            throw runtime_error("Unable to access LineSelector");
        }

        // Set Line Mode to Output
        CEnumerationPtr ptrLineMode = nodeMap.GetNode("LineMode");
        if (IsWritable(ptrLineMode)) {
            CEnumEntryPtr ptrOutput = ptrLineMode->GetEntryByName("Output");
            if (IsReadable(ptrOutput)) {
                ptrLineMode->SetIntValue(ptrOutput->GetValue());
            }
            else {
                throw runtime_error("Unable to set line mode to output");
            }
        }
        else {
            throw runtime_error("Unable to access LineMode");
        }
    }

    void createSignalFile()
    {
        // Create the signal file in the specified path
        string signal_file = fs::path(path).string() + "/rig_" + rig + "_camera_finished.signal";
        ofstream file(signal_file);
        file.close();
    }

    void setExposureTimeLowerLimit(double exposureTimeLowerLimit)
    {
        INodeMap& nodeMap = pCam->GetNodeMap();

        // Set ExposureAuto to Continuous
        CEnumerationPtr ptrExposureAuto = nodeMap.GetNode("ExposureAuto");
        if (IsWritable(ptrExposureAuto)) {
            CEnumEntryPtr ptrExposureAutoContinuous =
                ptrExposureAuto->GetEntryByName("Continuous");
            if (IsReadable(ptrExposureAutoContinuous)) {
                ptrExposureAuto->SetIntValue(ptrExposureAutoContinuous->GetValue());
            }
            else {
                throw runtime_error("Unable to set ExposureAuto to Continuous");
            }
        }
        else {
            throw runtime_error("Unable to access ExposureAuto");
        }

        // Set AutoExposureExposureTimeLowerLimit
        CFloatPtr ptrExposureTimeLowerLimit =
            nodeMap.GetNode("AutoExposureExposureTimeLowerLimit");
        if (!IsAvailable(ptrExposureTimeLowerLimit) || !IsWritable(ptrExposureTimeLowerLimit)) {
            throw runtime_error("Unable to access AutoExposureExposureTimeLowerLimit");
        }

        double minExposureTimeLowerLimit = ptrExposureTimeLowerLimit->GetMin();
        double maxExposureTimeLowerLimit = ptrExposureTimeLowerLimit->GetMax();

        if (exposureTimeLowerLimit < minExposureTimeLowerLimit)
            exposureTimeLowerLimit = minExposureTimeLowerLimit;
        else if (exposureTimeLowerLimit > maxExposureTimeLowerLimit)
            exposureTimeLowerLimit = maxExposureTimeLowerLimit;

        ptrExposureTimeLowerLimit->SetValue(exposureTimeLowerLimit);
    }
};

// Main function
int main(int argc, char** argv)
{
    string mouse_ID = "NoID";
    string date_time = "";
    string path = "";
    string serial_number = "";
    float FPS = 60.0f;
    int windowWidth = 800;  // Default window width
    int windowHeight = 600; // Default window height

    // Parse command-line arguments
    for (int i = 1; i < argc; i += 2) {
        string arg = argv[i];
        if (arg == "--id" && i + 1 < argc) {
            mouse_ID = argv[i + 1];
        }
        else if (arg == "--date" && i + 1 < argc) {
            date_time = argv[i + 1];
        }
        else if (arg == "--path" && i + 1 < argc) {
            path = argv[i + 1];
        }
        else if (arg == "--serial_number" && i + 1 < argc) {
            serial_number = argv[i + 1];
        }
        else if (arg == "--fps" && i + 1 < argc) {
            FPS = stof(argv[i + 1]);
        }
        else if (arg == "--windowWidth" && i + 1 < argc) {
            windowWidth = stoi(argv[i + 1]);
        }
        else if (arg == "--windowHeight" && i + 1 < argc) {
            windowHeight = stoi(argv[i + 1]);
        }
    }

    if (date_time.empty()) {
        auto now = system_clock::now();
        time_t now_time = system_clock::to_time_t(now);
        char buffer[80];
        tm localTime;
        localtime_s(&localTime, &now_time);
        strftime(buffer, sizeof(buffer), "%y%m%d_%H%M%S", &localTime);
        date_time = string(buffer);
    }

    if (path.empty()) {
        // Default path if none is provided
        path = "E:\\test_vid_output";  // Change this to your desired default path
        path += "\\" + date_time + "_" + mouse_ID;
    }

    // Create the output directory whichever way the path was arrived at. A path
    // given with --path used to be left alone, so the only sign that it did not
    // exist was the binary file failing to open several steps later; and the old
    // _mkdir call could not create a missing parent, nor tolerate the directory
    // already being there.
    try {
        fs::create_directories(path);
    }
    catch (const fs::filesystem_error& e) {
        cerr << "Error: unable to create output directory " << path << ": " << e.what() << endl;
        return -1;
    }
    if (!fs::is_directory(path)) {
        cerr << "Error: output path is not a directory: " << path << endl;
        return -1;
    }

    try {
        CameraRecorder camera(mouse_ID, date_time, path, serial_number, FPS, windowWidth, windowHeight);
        camera.startRecording(true, true);
    }
    catch (const std::exception& e) {
        cerr << "Error: " << e.what() << endl;
        return -1;
    }

    return 0;
}