#include "config.h"

#include <array>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

#if defined(_WIN32)
#include <windows.h>
#endif

namespace fs = std::filesystem;

namespace behaviour_camera {
namespace {

// Directory holding the running executable. Config sitting next to the .exe is the
// normal arrangement for a deployed rig, where there is no source tree to look in.
fs::path executableDirectory()
{
#if defined(_WIN32)
    std::array<wchar_t, MAX_PATH> buffer{};
    const DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (length > 0 && length < buffer.size()) {
        return fs::path(buffer.data()).parent_path();
    }
#endif
    return fs::current_path();
}

std::string environmentVariable(const char* name)
{
#if defined(_WIN32)
    // _dupenv_s rather than getenv: MSVC deprecates the latter, and this version
    // hands back a copy that cannot be invalidated by a later environment change.
    char* value = nullptr;
    size_t length = 0;
    if (_dupenv_s(&value, &length, name) == 0 && value != nullptr) {
        std::string result(value);
        std::free(value);
        return result;
    }
    return {};
#else
    const char* value = std::getenv(name);
    return value ? std::string(value) : std::string();
#endif
}

// Reads one setting out of a JSON object if it is present, leaving the existing
// value alone otherwise. This is what makes the layers additive: a per-camera block
// that only sets "fps" does not wipe out everything else.
template <typename T>
void readIfPresent(const nlohmann::json& object, const char* key, T& target)
{
    if (object.is_object() && object.contains(key) && !object.at(key).is_null()) {
        target = object.at(key).get<T>();
    }
}

void applyLayer(const nlohmann::json& layer, Settings& settings)
{
    if (!layer.is_object()) {
        return;
    }

    readIfPresent(layer, "rig", settings.rig);
    readIfPresent(layer, "output_root", settings.output_root);
    readIfPresent(layer, "fps", settings.fps);
    readIfPresent(layer, "exposure_lower_limit_us", settings.exposure_lower_limit_us);
    readIfPresent(layer, "strobe_line", settings.strobe_line);
    readIfPresent(layer, "stream_buffers", settings.stream_buffers);
    readIfPresent(layer, "display_fps", settings.display_fps);

    if (layer.contains("recording") && layer.at("recording").is_object()) {
        const nlohmann::json& recording = layer.at("recording");
        readIfPresent(recording, "mode", settings.recording_mode);

        if (recording.contains("video") && recording.at("video").is_object()) {
            const nlohmann::json& video = recording.at("video");
            readIfPresent(video, "ffmpeg", settings.video.ffmpeg);
            readIfPresent(video, "codec", settings.video.codec);
            readIfPresent(video, "preset", settings.video.preset);
            readIfPresent(video, "tune", settings.video.tune);
            readIfPresent(video, "qp", settings.video.qp);
            readIfPresent(video, "gop", settings.video.gop);
            readIfPresent(video, "container", settings.video.container);
            readIfPresent(video, "extra_args", settings.video.extra_args);
        }
    }

    if (layer.contains("window") && layer.at("window").is_object()) {
        const nlohmann::json& window = layer.at("window");
        readIfPresent(window, "width", settings.window_width);
        readIfPresent(window, "height", settings.window_height);
    }
}

} // namespace

nlohmann::json Settings::toJson() const
{
    return {
        { "rig", rig },
        { "output_root", output_root },
        { "fps", fps },
        { "exposure_lower_limit_us", exposure_lower_limit_us },
        { "strobe_line", strobe_line },
        { "stream_buffers", stream_buffers },
        { "window_width", window_width },
        { "window_height", window_height },
        { "display_fps", display_fps },
        { "recording_mode", recording_mode },
        { "video", {
            { "codec", video.codec },
            { "preset", video.preset },
            { "tune", video.tune },
            { "qp", video.qp },
            { "gop", video.gop },
            { "container", video.container },
        } },
    };
}

ConfigFile findConfig(const std::string& explicitPath)
{
    std::vector<fs::path> candidates;

    if (!explicitPath.empty()) {
        // Asked for by name: if it is not there, say so rather than quietly falling
        // back to defaults and recording with settings nobody chose.
        if (!fs::exists(explicitPath)) {
            throw std::runtime_error("Config file not found: " + explicitPath);
        }
        candidates.emplace_back(explicitPath);
    } else {
        const std::string fromEnvironment = environmentVariable("BEHAVIOUR_CAMERA_CONFIG");
        if (!fromEnvironment.empty()) {
            candidates.emplace_back(fromEnvironment);
        }

        const fs::path exeDir = executableDirectory();
        candidates.push_back(exeDir / "behaviour_camera.json");
        candidates.push_back(exeDir / "config" / "behaviour_camera.json");
        candidates.push_back(exeDir.parent_path() / "config" / "behaviour_camera.json");
        // Two levels up covers running straight out of the build tree, where the
        // executable sits in out/build/<preset>/ and the config is in the source root.
        candidates.push_back(exeDir.parent_path().parent_path().parent_path() / "config" / "behaviour_camera.json");
        candidates.push_back(fs::current_path() / "behaviour_camera.json");
        candidates.push_back(fs::current_path() / "config" / "behaviour_camera.json");
    }

    for (const fs::path& candidate : candidates) {
        std::error_code error;
        if (!fs::is_regular_file(candidate, error)) {
            continue;
        }

        std::ifstream stream(candidate);
        if (!stream) {
            continue;
        }

        try {
            ConfigFile result;
            result.path = candidate.string();
            // Comments are allowed: a rig config is edited by hand and benefits from
            // being annotated. The trailing arguments are (callback, allow_exceptions,
            // ignore_comments).
            result.contents = nlohmann::json::parse(stream, nullptr, true, true);
            return result;
        }
        catch (const nlohmann::json::parse_error& e) {
            throw std::runtime_error("Could not parse " + candidate.string() + ": " + e.what());
        }
    }

    return {};
}

Settings resolveSettings(const ConfigFile& config, const std::string& serial)
{
    Settings settings;                      // layer 1: built-in defaults

    if (config.found() && config.contents.is_object()) {
        if (config.contents.contains("defaults")) {
            applyLayer(config.contents.at("defaults"), settings);   // layer 2
        }

        if (config.contents.contains("cameras") && config.contents.at("cameras").is_object()) {
            const nlohmann::json& cameras = config.contents.at("cameras");
            if (cameras.contains(serial)) {
                applyLayer(cameras.at(serial), settings);           // layer 3
            }
        }
    }

    // A camera nobody has named still records perfectly well; it just needs
    // something unique to call itself, since the rig name ends up in the signal
    // filenames that the launcher watches for.
    if (settings.rig.empty()) {
        settings.rig = "cam_" + serial;
    }

    return settings;
}

} // namespace behaviour_camera
