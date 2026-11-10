#pragma once

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

#include <httplib.h>

// API de streaming (SPEC.md §4.4 y §10.4).
namespace tecflix::streaming {

namespace fs = std::filesystem;

struct VideoInfo {
    std::string authorId;
    std::string status;
    std::uint64_t sizeBytes = 0;
};

// Lo que streaming necesita del API de metadata. Interfaz para poder probar sin red.
class MetadataClient {
public:
    virtual ~MetadataClient() = default;
    // El video, solo si existe y es visible para el dueño de `userToken`.
    virtual std::optional<VideoInfo> getVideo(const std::string& id,
                                              const std::string& userToken) = 0;
    virtual bool reportStatus(const std::string& id, const std::string& status,
                              double durationSeconds, std::uint64_t sizeBytes) = 0;
};

class HttpMetadataClient : public MetadataClient {
public:
    HttpMetadataClient(std::string baseUrl, std::string jwtSecret);
    std::optional<VideoInfo> getVideo(const std::string& id,
                                      const std::string& userToken) override;
    bool reportStatus(const std::string& id, const std::string& status, double durationSeconds,
                      std::uint64_t sizeBytes) override;

private:
    std::string baseUrl_;
    std::string jwtSecret_;
};

class Transcoder {
public:
    virtual ~Transcoder() = default;
    // Genera `outputDir/manifest.mpd` + segmentos. Devuelve la duración en segundos,
    // o nullopt si el archivo no es un video válido o la transcodificación falla.
    virtual std::optional<double> transcode(const fs::path& source, const fs::path& outputDir) = 0;
};

class FfmpegTranscoder : public Transcoder {
public:
    std::optional<double> transcode(const fs::path& source, const fs::path& outputDir) override;
    static std::string buildCommand(const fs::path& source, const fs::path& outputDir);
    static std::optional<double> probeDuration(const fs::path& source);
};

// Ubicación en disco de cada video: <root>/<id>/source y <root>/<id>/dash/.
class VideoStore {
public:
    explicit VideoStore(fs::path root) : root_(std::move(root)) {}
    fs::path sourcePath(const std::string& id) const { return root_ / id / "source"; }
    fs::path dashDir(const std::string& id) const { return root_ / id / "dash"; }
    void remove(const std::string& id) const { fs::remove_all(root_ / id); }

private:
    fs::path root_;
};

// Cola de transcodificación: un hilo trabajador, un video a la vez.
class TranscodeQueue {
public:
    TranscodeQueue(const VideoStore& store, Transcoder& transcoder, MetadataClient& metadata);
    void enqueue(std::string id);
    void waitIdle();  // bloquea hasta que no quede trabajo (para pruebas y apagado limpio)

private:
    void run(const std::stop_token& stop);
    void process(const std::string& id);

    const VideoStore& store_;
    Transcoder& transcoder_;
    MetadataClient& metadata_;
    std::mutex mutex_;
    std::condition_variable_any changed_;
    std::deque<std::string> pending_;
    bool busy_ = false;
    // shortcut: los trabajos en cola se pierden si el proceso muere (quedan en "processing");
    // en V2.0 hay que re-encolarlos al arrancar.
    std::jthread worker_;  // último miembro: se detiene antes de destruir lo demás
};

struct Config {
    std::string jwtSecret;
    std::string instanceId;
};

void registerRoutes(httplib::Server& server, const Config& config, const VideoStore& store,
                    MetadataClient& metadata, TranscodeQueue& queue);

}  // namespace tecflix::streaming
