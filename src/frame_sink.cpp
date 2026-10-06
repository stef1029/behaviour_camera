#include "frame_sink.h"

#include <array>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>

#if defined(_WIN32)
#include <windows.h>
#endif

namespace fs = std::filesystem;

namespace behaviour_camera {
namespace {

// ---------------------------------------------------------------------------
// Raw
// ---------------------------------------------------------------------------

class RawFileSink : public FrameSink
{
public:
    explicit RawFileSink(const fs::path& outputPath)
        : path_(outputPath), file_(outputPath, std::ios::binary | std::ios::out)
    {
        if (!file_.is_open()) {
            throw std::runtime_error("Could not open the binary video file for writing: " +
                                     outputPath.string());
        }
    }

    bool write(const void* data, size_t bytes) override
    {
        file_.write(static_cast<const char*>(data), static_cast<std::streamsize>(bytes));
        if (!file_.good()) {
            return false;
        }
        if (frameBytes_ == 0) {
            frameBytes_ = bytes;
        }
        ++frames_;
        return true;
    }

    bool finish() override
    {
        file_.flush();
        file_.close();
        return !file_.bad();
    }

    // Measured from the finished file rather than returning our own counter, which
    // would only confirm that we counted what we counted. A short final write shows
    // up here as a file that does not divide into whole frames.
    int64_t frameCount() const override
    {
        if (frameBytes_ == 0) {
            return frames_;
        }
        std::error_code error;
        const auto size = fs::file_size(path_, error);
        if (error) {
            return -1;
        }
        return static_cast<int64_t>(size / frameBytes_);
    }
    fs::path path() const override { return path_; }
    std::string describe() const override { return "raw frames to " + path_.filename().string(); }

private:
    fs::path path_;
    std::ofstream file_;
    int64_t frames_ = 0;
    size_t frameBytes_ = 0;
};

#if defined(_WIN32)

// ---------------------------------------------------------------------------
// Video, via a pipe to ffmpeg
// ---------------------------------------------------------------------------

// Quotes one argument for a Windows command line. CreateProcess takes a single
// string, and the output paths contain spaces often enough that getting this wrong
// would be a reliable way to lose a session.
std::string quoteArgument(const std::string& argument)
{
    if (!argument.empty() && argument.find_first_of(" \t\"") == std::string::npos) {
        return argument;
    }

    std::string quoted = "\"";
    for (auto it = argument.begin();; ++it) {
        size_t backslashes = 0;
        while (it != argument.end() && *it == '\\') {
            ++it;
            ++backslashes;
        }

        if (it == argument.end()) {
            // Backslashes before the closing quote must be doubled, or they escape it.
            quoted.append(backslashes * 2, '\\');
            break;
        }
        if (*it == '"') {
            quoted.append(backslashes * 2 + 1, '\\');
        } else {
            quoted.append(backslashes, '\\');
        }
        quoted.push_back(*it);
    }
    quoted.push_back('"');
    return quoted;
}

std::string buildCommandLine(const std::vector<std::string>& arguments)
{
    std::string line;
    for (size_t i = 0; i < arguments.size(); ++i) {
        if (i > 0) {
            line.push_back(' ');
        }
        line += quoteArgument(arguments[i]);
    }
    return line;
}

class VideoSink : public FrameSink
{
public:
    VideoSink(const fs::path& outputPath, const VideoSettings& settings,
              int width, int height, double fps)
        : path_(outputPath), settings_(settings)
    {
        const std::vector<std::string> arguments = buildArguments(outputPath, settings,
                                                                  width, height, fps);
        commandLine_ = buildCommandLine(arguments);
        start();
    }

    ~VideoSink() override
    {
        // Make sure the child is not left running if finish() was never reached.
        closePipe();
        if (process_ != nullptr) {
            WaitForSingleObject(process_, 5000);
            CloseHandle(process_);
            process_ = nullptr;
        }
    }

    bool write(const void* data, size_t bytes) override
    {
        const char* cursor = static_cast<const char*>(data);
        size_t remaining = bytes;

        while (remaining > 0) {
            DWORD written = 0;
            const DWORD chunk = static_cast<DWORD>(
                remaining > 0x10000000 ? 0x10000000 : remaining);

            if (!WriteFile(pipe_, cursor, chunk, &written, nullptr) || written == 0) {
                // Almost always means ffmpeg exited: a broken pipe. Report what it
                // said rather than just the write failure.
                std::cerr << "Error: writing to the encoder failed. " << exitDescription() << std::endl;
                return false;
            }
            cursor += written;
            remaining -= written;
        }

        ++frames_;
        return true;
    }

    bool finish() override
    {
        // Closing stdin tells ffmpeg the stream has ended, which is what makes it
        // flush and finalise the container. Killing it here would leave a truncated
        // file instead.
        closePipe();

        if (process_ == nullptr) {
            return false;
        }

        if (WaitForSingleObject(process_, 60000) != WAIT_OBJECT_0) {
            std::cerr << "Error: the encoder did not finish within 60 s; terminating it. "
                      << "The video may be incomplete." << std::endl;
            TerminateProcess(process_, 1);
            WaitForSingleObject(process_, 5000);
            return false;
        }

        DWORD exitCode = 1;
        GetExitCodeProcess(process_, &exitCode);
        CloseHandle(process_);
        process_ = nullptr;

        if (exitCode != 0) {
            std::cerr << "Error: the encoder exited with code " << exitCode << std::endl;
            return false;
        }
        return true;
    }

    // Read back from the finished file rather than counted on the way in, so this
    // is an independent check that every frame really was encoded.
    int64_t frameCount() const override { return countFramesInFile(); }

    fs::path path() const override { return path_; }

    std::string describe() const override
    {
        std::ostringstream text;
        text << settings_.codec << " qp" << settings_.qp << " gop" << settings_.gop
             << " to " << path_.filename().string();
        return text.str();
    }

private:
    static std::vector<std::string> buildArguments(const fs::path& outputPath,
                                                   const VideoSettings& settings,
                                                   int width, int height, double fps)
    {
        std::ostringstream size;
        size << width << "x" << height;
        std::ostringstream rate;
        rate << fps;

        std::vector<std::string> arguments = {
            settings.ffmpeg,
            "-hide_banner",
            "-loglevel", "error",
            "-y",
            // Input: exactly what the camera produces, one byte per pixel.
            "-f", "rawvideo",
            "-pix_fmt", "gray",
            "-s", size.str(),
            "-r", rate.str(),
            "-i", "-",
            "-an",
            "-c:v", settings.codec,
            "-preset", settings.preset,
        };

        if (!settings.tune.empty()) {
            arguments.push_back("-tune");
            arguments.push_back(settings.tune);
        }

        arguments.push_back("-rc");
        arguments.push_back("constqp");
        arguments.push_back("-qp");
        arguments.push_back(std::to_string(settings.qp));
        arguments.push_back("-g");
        arguments.push_back(std::to_string(settings.gop));

        // B-frames off. NVENC rejects a GOP shorter than its B-frame count, so an
        // all-intra encode fails without this, and B-frames buy little on footage
        // that barely changes.
        arguments.push_back("-bf");
        arguments.push_back("0");

        arguments.push_back("-pix_fmt");
        arguments.push_back("yuv420p");

        for (const std::string& extra : settings.extra_args) {
            arguments.push_back(extra);
        }

        arguments.push_back(outputPath.string());
        return arguments;
    }

    void start()
    {
        SECURITY_ATTRIBUTES security{};
        security.nLength = sizeof(security);
        security.bInheritHandle = TRUE;

        HANDLE readEnd = nullptr;
        if (!CreatePipe(&readEnd, &pipe_, &security, 1 << 22)) {
            throw std::runtime_error("Could not create a pipe for the encoder");
        }
        // Only the child should inherit the read end; our write end must not leak
        // into it, or the pipe never reports end-of-file.
        SetHandleInformation(pipe_, HANDLE_FLAG_INHERIT, 0);

        STARTUPINFOA startup{};
        startup.cb = sizeof(startup);
        startup.dwFlags = STARTF_USESTDHANDLES;
        startup.hStdInput = readEnd;
        startup.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
        startup.hStdError = GetStdHandle(STD_ERROR_HANDLE);

        PROCESS_INFORMATION info{};
        std::string mutableCommandLine = commandLine_;

        const BOOL started = CreateProcessA(nullptr, mutableCommandLine.data(), nullptr, nullptr,
                                            TRUE, 0, nullptr, nullptr, &startup, &info);
        CloseHandle(readEnd);

        if (!started) {
            CloseHandle(pipe_);
            pipe_ = INVALID_HANDLE_VALUE;
            throw std::runtime_error(
                "Could not start the encoder. Is ffmpeg installed and on PATH?\n"
                "  tried: " + commandLine_);
        }

        CloseHandle(info.hThread);
        process_ = info.hProcess;

        // ffmpeg rejects bad arguments immediately, so a short wait here turns a
        // silent failure into a clear one before any frames are recorded.
        if (WaitForSingleObject(process_, 300) == WAIT_OBJECT_0) {
            DWORD exitCode = 1;
            GetExitCodeProcess(process_, &exitCode);
            CloseHandle(process_);
            process_ = nullptr;
            closePipe();
            throw std::runtime_error(
                "The encoder exited immediately with code " + std::to_string(exitCode) +
                ". The error above is from ffmpeg.\n  command: " + commandLine_);
        }
    }

    void closePipe()
    {
        if (pipe_ != INVALID_HANDLE_VALUE) {
            CloseHandle(pipe_);
            pipe_ = INVALID_HANDLE_VALUE;
        }
    }

    std::string exitDescription() const
    {
        if (process_ == nullptr) {
            return "The encoder is no longer running.";
        }
        DWORD exitCode = STILL_ACTIVE;
        GetExitCodeProcess(process_, &exitCode);
        if (exitCode == STILL_ACTIVE) {
            return "The encoder is still running, so the pipe failed for another reason.";
        }
        return "The encoder exited with code " + std::to_string(exitCode) + ".";
    }

    // Asks ffprobe how many frames the container holds. Returns -1 if that cannot
    // be determined, which is treated as "unknown" rather than "wrong".
    //
    // Counts packets rather than reading stream=nb_frames, which Matroska leaves
    // unset - so the obvious version of this check returned "unknown" every time and
    // silently verified nothing. One video packet is one frame, and counting them
    // does not decode anything: measured at 0.09 s against 1.6 s for a full decode.
    int64_t countFramesInFile() const
    {
        const std::string command =
            buildCommandLine({ "ffprobe", "-v", "error", "-select_streams", "v:0",
                               "-count_packets", "-show_entries", "stream=nb_read_packets",
                               "-of", "default=nk=1:nw=1", path_.string() });

        std::string output;
        if (FILE* pipe = _popen(command.c_str(), "r")) {
            std::array<char, 128> buffer{};
            while (fgets(buffer.data(), static_cast<int>(buffer.size()), pipe) != nullptr) {
                output += buffer.data();
            }
            _pclose(pipe);
        }

        try {
            return output.empty() ? -1 : std::stoll(output);
        }
        catch (const std::exception&) {
            return -1;
        }
    }

    fs::path path_;
    VideoSettings settings_;
    std::string commandLine_;
    HANDLE pipe_ = INVALID_HANDLE_VALUE;
    HANDLE process_ = nullptr;
    int64_t frames_ = 0;
};

#endif // _WIN32

} // namespace

std::unique_ptr<FrameSink> makeRawSink(const fs::path& outputPath)
{
    return std::make_unique<RawFileSink>(outputPath);
}

std::unique_ptr<FrameSink> makeVideoSink(const fs::path& outputStem,
                                         const VideoSettings& settings,
                                         int width, int height, double fps)
{
#if defined(_WIN32)
    fs::path output = outputStem;
    output += "." + settings.container;
    return std::make_unique<VideoSink>(output, settings, width, height, fps);
#else
    (void)outputStem; (void)settings; (void)width; (void)height; (void)fps;
    throw std::runtime_error("Video encoding is only implemented for Windows");
#endif
}

} // namespace behaviour_camera
