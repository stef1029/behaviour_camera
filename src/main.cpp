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
#include <opencv2/opencv.hpp>
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

        // Set serial number based on cam_no
        if (camSerial == "22181614") { // rig 1
            max_FPS = 170.0;
            rig = "1";
        }
        else if (camSerial == "20530175") {  // rig 2
            max_FPS = 170.0;
            rig = "2";
        }
        else if (camSerial == "24174008") {  // rig 3
            max_FPS = 170.0;
            rig = "3";
        }
        else if (camSerial == "24243513") { // rig 4
            max_FPS = 170.0;
            rig = "4";
        }
        else if (camSerial == "24174020") { // openfield
            max_FPS = 170.0;
            rig = "openfield";
        }
        else if (camSerial == "23606054") {  // Colour camera
            max_FPS = 170.0;
            rig = "colour_camera";
        }
        else if (camSerial == "21423798") {  // 6.3MP camera
            max_FPS = 59.60;
            rig = "6MP3_camera";
        }
        else {
            throw runtime_error("Invalid camera number");
        }

        // Limit FPS to max amount of camera
        if (FPS > max_FPS) {
            FPS = max_FPS;
        }

        windowTitle << "Rig " << rig << ". Press 'Esc' to stop session.";
        title = windowTitle.str();

        // Use GetBySerial to get the camera
        pCam = camList.GetBySerial(camSerial);

        if (!pCam) {
            cerr << "Error: Camera can't open\nexit" << endl;
            throw runtime_error("Camera can't open");
        }

        pCam->Init();

        // Print host controller information
        cout << "===== Host Controller Information =====" << endl;
        printHostControllerInfo();
        cout << "=======================================" << endl;

        setCameraFrameRate(FPS);    // Set the frame rate
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
    ~CameraRecorder()
    {
        if (pCam) {
            pCam->EndAcquisition();
            pCam->DeInit();
            pCam = nullptr;
        }
        if (imageFile.is_open()) {
            imageFile.close();
        }
        system->ReleaseInstance();
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
    float max_FPS;
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

        if (pCam) {
            pCam->EndAcquisition();
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

            pCam->EndAcquisition();
            std::this_thread::sleep_for(std::chrono::milliseconds(500));

            // Reset camera settings
            pCam->DeInit();
            std::this_thread::sleep_for(std::chrono::milliseconds(500));

            pCam->Init();
            setCameraFrameRate(FPS);
            setGPIOLine2ToOutput();
            setExposureTimeLowerLimit(4000.0);

            pCam->BeginAcquisition();

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

    void setCameraFrameRate(double frameRate)
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
        if (IsWritable(ptrFrameRate)) {
            ptrFrameRate->SetValue(frameRate);
        }
        else {
            throw runtime_error("Unable to set frame rate");
        }
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

    void printHostControllerInfo()
    {
        // Access the Transport Layer node map
        INodeMap& TLNodeMap = pCam->GetTLDeviceNodeMap();

        // Get HostAdapterName
        CStringPtr ptrHostAdapterName = TLNodeMap.GetNode("HostAdapterName");
        if (IsReadable(ptrHostAdapterName))
        {
            cout << "Host Adapter Name: " << ptrHostAdapterName->GetValue() << endl;
        }
        else
        {
            cout << "Host Adapter Name: Not available" << endl;
        }

        // Get HostAdapterVendor
        CStringPtr ptrHostAdapterVendor = TLNodeMap.GetNode("HostAdapterVendor");
        if (IsReadable(ptrHostAdapterVendor))
        {
            cout << "Host Adapter Vendor: " << ptrHostAdapterVendor->GetValue() << endl;
        }
        else
        {
            cout << "Host Adapter Vendor: Not available" << endl;
        }

        // Get HostAdapterDriverVersion
        CStringPtr ptrHostAdapterDriverVersion = TLNodeMap.GetNode("HostAdapterDriverVersion");
        if (IsReadable(ptrHostAdapterDriverVersion))
        {
            cout << "Host Adapter Driver Version: " << ptrHostAdapterDriverVersion->GetValue() << endl;
        }
        else
        {
            cout << "Host Adapter Driver Version: Not available" << endl;
        }
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
        if (_mkdir(path.c_str()) != 0) {
            cerr << "Error: Unable to create directory " << path << endl;
            return -1;
        }
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