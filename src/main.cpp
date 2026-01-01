#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace fs = std::filesystem;

namespace {

struct CommandResult {
    int exitCode{};
    std::string output;
};

bool isWindows() {
#ifdef _WIN32
    return true;
#else
    return false;
#endif
}

std::string shellEscape(const std::string& value) {
    std::string escaped = "'";
    for (char c : value) {
        if (c == '\'') {
            escaped += "'\"'\"'";
        } else {
            escaped.push_back(c);
        }
    }
    escaped.push_back('\'');
    return escaped;
}

CommandResult runCommand(const std::string& command) {
    std::array<char, 256> buffer{};
    std::string result;

#ifdef _WIN32
    FILE* pipe = _popen((command + " 2>&1").c_str(), "r");
#else
    FILE* pipe = popen((command + " 2>&1").c_str(), "r");
#endif
    if (!pipe) {
        throw std::runtime_error("Failed to open pipe for command: " + command);
    }

    while (fgets(buffer.data(), buffer.size(), pipe) != nullptr) {
        result += buffer.data();
    }

#ifdef _WIN32
    int exitCode = _pclose(pipe);
#else
    int exitCode = pclose(pipe);
#endif
    return {exitCode, result};
}

fs::path executableDir(const char* argv0) {
    fs::path execPath(argv0);
    if (execPath.is_relative()) {
        execPath = fs::current_path() / execPath;
    }

    std::error_code ec;
    auto canonicalPath = fs::weakly_canonical(execPath, ec);
    if (!ec) {
        execPath = canonicalPath;
    }
    return execPath.parent_path();
}

bool isExecutable(const fs::path& p) {
    std::error_code ec;
    auto status = fs::status(p, ec);
    if (ec || !fs::is_regular_file(status)) {
        return false;
    }
#ifdef _WIN32
    // On Windows, presence of the file is enough; execution permission is implicit.
    return true;
#else
    return (status.permissions() & fs::perms::owner_exec) != fs::perms::none;
#endif
}

fs::path findTool(const fs::path& baseDir, const std::string& name) {
    std::vector<fs::path> candidates;

    auto maybeAdd = [&](const fs::path& p) {
        candidates.push_back(p);
    };

    const std::string exeSuffix = isWindows() ? ".exe" : "";

    // Local project structure candidates: same dir as executable, bin/, third_party/...
    maybeAdd(baseDir / (name + exeSuffix));
    maybeAdd(baseDir / "bin" / (name + exeSuffix));
    maybeAdd(baseDir / "third_party" / name / (name + exeSuffix));
    maybeAdd(baseDir / "third_party" / "bin" / (name + exeSuffix));

    for (const auto& candidate : candidates) {
        if (isExecutable(candidate)) {
            return candidate;
        }
    }

    throw std::runtime_error("Required tool not found in project folders: " + name);
}

void requireCommand(const fs::path& commandPath, const std::string& versionFlag = "-version") {
    auto res = runCommand(shellEscape(commandPath.string()) + " " + versionFlag);
    if (res.exitCode != 0) {
        throw std::runtime_error("Required command '" + commandPath.string() + "' is not available.\nOutput:\n" +
                                 res.output);
    }
}

void requireNvidiaGpu() {
    auto res = runCommand("nvidia-smi --query-gpu=name --format=csv,noheader");
    if (res.exitCode != 0 || res.output.empty()) {
        throw std::runtime_error("No NVIDIA GPU detected. The application requires an NVIDIA GPU to run.");
    }

    std::istringstream stream(res.output);
    std::string gpuName;
    std::getline(stream, gpuName);
    std::cout << "Detected NVIDIA GPU: " << gpuName << "\n";
}

struct VideoMetadata {
    int width{};
    int height{};
    double fps{};
    std::string fpsRaw;
    double duration{};
    long long totalFrames{};
};

double parseFrameRate(const std::string& value) {
    if (value.empty()) {
        return 0.0;
    }

    auto slashPos = value.find('/');
    if (slashPos == std::string::npos) {
        return std::stod(value);
    }

    double numerator = std::stod(value.substr(0, slashPos));
    double denominator = std::stod(value.substr(slashPos + 1));
    if (denominator == 0.0) {
        return 0.0;
    }
    return numerator / denominator;
}

long long safeParseLong(const std::string& token) {
    if (token == "N/A" || token.empty()) {
        return -1;
    }
    return std::stoll(token);
}

double safeParseDouble(const std::string& token) {
    if (token == "N/A" || token.empty()) {
        return 0.0;
    }
    return std::stod(token);
}

std::vector<std::string> splitCsvLine(const std::string& line) {
    std::vector<std::string> tokens;
    std::stringstream ss(line);
    std::string item;
    while (std::getline(ss, item, ',')) {
        tokens.push_back(item);
    }
    return tokens;
}

VideoMetadata probeVideo(const fs::path& ffprobe, const fs::path& input) {
    std::ostringstream cmd;
    cmd << shellEscape(ffprobe.string())
        << " -v error -select_streams v:0 -count_frames "
        << "-show_entries stream=nb_read_frames,nb_frames,width,height,avg_frame_rate,duration "
        << "-of csv=p=0 " << shellEscape(input.string());

    auto res = runCommand(cmd.str());
    if (res.exitCode != 0) {
        throw std::runtime_error("Failed to probe video metadata:\n" + res.output);
    }

    std::istringstream output(res.output);
    std::string line;
    std::getline(output, line);
    auto tokens = splitCsvLine(line);
    if (tokens.size() < 6) {
        throw std::runtime_error("Unexpected ffprobe output:\n" + res.output);
    }

    VideoMetadata meta{};
    meta.width = static_cast<int>(safeParseLong(tokens[0]));
    meta.height = static_cast<int>(safeParseLong(tokens[1]));
    auto nbReadFrames = safeParseLong(tokens[2]);
    auto nbFrames = safeParseLong(tokens[3]);
    meta.fpsRaw = tokens[4];
    meta.fps = parseFrameRate(meta.fpsRaw);
    meta.duration = safeParseDouble(tokens[5]);

    if (nbReadFrames > 0) {
        meta.totalFrames = nbReadFrames;
    } else if (nbFrames > 0) {
        meta.totalFrames = nbFrames;
    } else if (meta.duration > 0.0 && meta.fps > 0.0) {
        meta.totalFrames = static_cast<long long>(meta.duration * meta.fps + 0.5);
    } else {
        meta.totalFrames = 0;
    }

    return meta;
}

void ensureDirectory(const fs::path& path) {
    fs::create_directories(path);
}

void extractAudio(const fs::path& ffmpeg, const fs::path& input, const fs::path& output) {
    std::ostringstream cmd;
    cmd << shellEscape(ffmpeg.string()) << " -y -i " << shellEscape(input.string()) << " -vn -acodec copy "
        << shellEscape(output.string());
    auto res = runCommand(cmd.str());
    if (res.exitCode != 0) {
        std::cout << "No audio track was extracted (audio will be omitted in the final render).\n";
    } else {
        std::cout << "Audio extracted to " << output << "\n";
    }
}

void extractFrames(const fs::path& ffmpeg, const fs::path& input, const fs::path& outputDir) {
    ensureDirectory(outputDir);
    std::ostringstream cmd;
    cmd << shellEscape(ffmpeg.string()) << " -y -i " << shellEscape(input.string()) << " -vsync 0 "
        << outputDir.string() << "/frame_%08d.png";
    auto res = runCommand(cmd.str());
    if (res.exitCode != 0) {
        throw std::runtime_error("Failed to extract frames:\n" + res.output);
    }
}

void printProgress(const std::string& label, std::size_t completed, std::size_t total) {
    double percent = total > 0 ? (static_cast<double>(completed) / static_cast<double>(total)) * 100.0 : 0.0;
    std::cout << "\r" << label << " " << completed << "/" << total << " (" << std::fixed << std::setprecision(1)
              << percent << "%)" << std::flush;
}

void upscaleFrames(const fs::path& realesrgan, const fs::path& inputDir, const fs::path& outputDir, std::size_t totalFrames) {
    ensureDirectory(outputDir);

    std::vector<fs::path> frames;
    for (const auto& entry : fs::directory_iterator(inputDir)) {
        if (entry.is_regular_file()) {
            frames.push_back(entry.path());
        }
    }

    std::sort(frames.begin(), frames.end());
    if (frames.empty()) {
        throw std::runtime_error("No frames found to upscale.");
    }

    const std::size_t total = totalFrames > 0 ? totalFrames : frames.size();
    std::size_t processed = 0;

    for (const auto& frame : frames) {
        fs::path outputFrame = outputDir / frame.filename();
        std::ostringstream cmd;
        cmd << shellEscape(realesrgan.string()) << " -i " << shellEscape(frame.string()) << " -o "
            << shellEscape(outputFrame.string()) << " -n realesrgan-x4plus -s 4 -g 0";
        auto res = runCommand(cmd.str());
        if (res.exitCode != 0) {
            throw std::runtime_error("Real-ESRGAN failed on frame " + frame.string() + ":\n" + res.output);
        }

        ++processed;
        printProgress("Upscaling frames:", processed, total);
    }
    std::cout << "\n";
}

std::string buildScaleFilter() {
    std::ostringstream filter;
    filter << "scale='min(2560,iw)':'min(1440,ih)':force_original_aspect_ratio=decrease";
    filter << ",scale=trunc(iw/2)*2:trunc(ih/2)*2";
    return filter.str();
}

void assembleVideo(const fs::path& framesDir,
                   const fs::path& audioFile,
                   const fs::path& outputFile,
                   const std::string& fpsRaw,
                   bool hasAudio,
                   const fs::path& ffmpeg) {
    std::ostringstream cmd;
    cmd << shellEscape(ffmpeg.string()) << " -y -framerate " << (fpsRaw.empty() ? "30" : fpsRaw)
        << " -i " << shellEscape((framesDir / "frame_%08d.png").string()) << " ";

    if (hasAudio) {
        cmd << "-i " << shellEscape(audioFile.string()) << " -map 0:v:0 -map 1:a:0 ";
    } else {
        cmd << "-map 0:v:0 ";
    }

    cmd << "-vf \"" << buildScaleFilter() << "\" "
        << "-c:v h264_nvenc -preset p3 -pix_fmt yuv420p ";

    if (hasAudio) {
        cmd << "-c:a copy ";
    }

    cmd << shellEscape(outputFile.string());

    auto res = runCommand(cmd.str());
    if (res.exitCode != 0) {
        throw std::runtime_error("Failed to assemble video:\n" + res.output);
    }
}

struct UpscaleConfig {
    fs::path input;
    fs::path output;
    fs::path workspace;
    fs::path execDir;
    fs::path ffmpeg;
    fs::path ffprobe;
    fs::path realesrgan;
};

UpscaleConfig parseArgs(int argc, char** argv) {
    if (argc < 3) {
        throw std::runtime_error("Usage: icecale <input_video> <output_video>");
    }

    UpscaleConfig cfg;
    cfg.input = fs::absolute(argv[1]);
    cfg.output = fs::absolute(argv[2]);
    cfg.workspace = fs::temp_directory_path() / "icecale-work";
    cfg.execDir = executableDir(argv[0]);
    return cfg;
}

}  // namespace

int main(int argc, char** argv) {
    try {
        auto config = parseArgs(argc, argv);

        if (!fs::exists(config.input)) {
            throw std::runtime_error("Input file does not exist: " + config.input.string());
        }

        std::cout << "Verifying environment...\n";
        requireNvidiaGpu();
        config.ffmpeg = findTool(config.execDir, "ffmpeg");
        config.ffprobe = findTool(config.execDir, "ffprobe");
        config.realesrgan = findTool(config.execDir, "realesrgan-ncnn-vulkan");

        requireCommand(config.ffmpeg);
        requireCommand(config.ffprobe);
        requireCommand(config.realesrgan, "-h");

        std::cout << "Probing input video...\n";
        auto metadata = probeVideo(config.ffprobe, config.input);
        std::cout << "Resolution: " << metadata.width << "x" << metadata.height << ", FPS: "
                  << (metadata.fpsRaw.empty() ? std::to_string(metadata.fps) : metadata.fpsRaw)
                  << ", Frames: " << metadata.totalFrames << "\n";

        const fs::path framesDir = config.workspace / "frames_raw";
        const fs::path upscaledDir = config.workspace / "frames_upscaled";
        const fs::path audioFile = config.workspace / "audio.mka";

        std::cout << "Extracting audio (if present)...\n";
        extractAudio(config.ffmpeg, config.input, audioFile);
        bool hasAudio = fs::exists(audioFile) && fs::file_size(audioFile) > 0;

        std::cout << "Extracting frames...\n";
        extractFrames(config.ffmpeg, config.input, framesDir);

        std::cout << "Upscaling with Real-ESRGAN (x4, capped to 1440p output)...\n";
        upscaleFrames(config.realesrgan, framesDir, upscaledDir, static_cast<std::size_t>(metadata.totalFrames));

        std::cout << "Assembling final video with resolution capped at 1440p...\n";
        assembleVideo(upscaledDir, audioFile, config.output, metadata.fpsRaw, hasAudio, config.ffmpeg);

        std::cout << "Upscaled video saved to: " << config.output << "\n";
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "Error: " << ex.what() << "\n";
        return 1;
    }
}
