#include <array>
#include <cstdio>
#include <cstdlib>
#include <memory>

#include "streaming.hpp"

namespace tecflix::streaming {

namespace {
// Las rutas las genera el servicio (DATA_DIR + id hexadecimal validado), nunca el usuario.
std::string quoted(const fs::path& path) {
    return "'" + path.string() + "'";
}
}  // namespace

std::string FfmpegTranscoder::buildCommand(const fs::path& source, const fs::path& outputDir) {
    return "ffmpeg -nostdin -loglevel error -y -i " + quoted(source) +
           // Dos calidades de video (360p y 720p) + audio opcional.
           " -map 0:v:0 -map 0:v:0 -map 0:a:0?"
           " -c:v libx264 -preset veryfast -pix_fmt yuv420p"
           " -filter:v:0 scale=-2:360 -b:v:0 800k"
           " -filter:v:1 scale=-2:720 -b:v:1 2500k"
           // Keyframes fijos: los segmentos de ambas calidades quedan alineados
           // y el reproductor puede cambiar de calidad en cualquier corte.
           " -g 48 -keyint_min 48 -sc_threshold 0"
           " -c:a aac -b:a 128k"
           " -seg_duration 4 -use_template 1 -use_timeline 1"
           " -init_seg_name 'init-$RepresentationID$.m4s'"
           " -media_seg_name 'chunk-$RepresentationID$-$Number%05d$.m4s'"
           " -f dash " +
           quoted(outputDir / "manifest.mpd");
}

std::optional<double> FfmpegTranscoder::probeDuration(const fs::path& source) {
    const std::string command =
        "ffprobe -v error -show_entries format=duration -of csv=p=0 " + quoted(source) +
        " 2>/dev/null";
    const std::unique_ptr<FILE, int (*)(FILE*)> pipe(popen(command.c_str(), "r"), pclose);
    std::array<char, 64> buffer{};
    if (!pipe || fgets(buffer.data(), static_cast<int>(buffer.size()), pipe.get()) == nullptr) {
        return std::nullopt;
    }
    char* end = nullptr;
    const double seconds = std::strtod(buffer.data(), &end);
    return (end != buffer.data() && seconds > 0) ? std::optional<double>(seconds) : std::nullopt;
}

std::optional<double> FfmpegTranscoder::transcode(const fs::path& source,
                                                  const fs::path& outputDir) {
    const auto duration = probeDuration(source);
    if (!duration) {
        return std::nullopt;
    }
    fs::create_directories(outputDir);
    if (std::system(buildCommand(source, outputDir).c_str()) != 0) {
        return std::nullopt;
    }
    return duration;
}

}  // namespace tecflix::streaming
