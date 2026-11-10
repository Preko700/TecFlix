#include "streaming.hpp"

#include <fstream>

#include "tecflix/http.hpp"

namespace tecflix::streaming {

TranscodeQueue::TranscodeQueue(const VideoStore& store, Transcoder& transcoder,
                               MetadataClient& metadata)
    : store_(store),
      transcoder_(transcoder),
      metadata_(metadata),
      worker_([this](const std::stop_token& stop) { run(stop); }) {}

void TranscodeQueue::enqueue(std::string id) {
    {
        const std::lock_guard lock(mutex_);
        pending_.push_back(std::move(id));
    }
    changed_.notify_all();
}

void TranscodeQueue::waitIdle() {
    std::unique_lock lock(mutex_);
    changed_.wait(lock, [this] { return pending_.empty() && !busy_; });
}

void TranscodeQueue::run(const std::stop_token& stop) {
    while (true) {
        std::string id;
        {
            std::unique_lock lock(mutex_);
            if (!changed_.wait(lock, stop, [this] { return !pending_.empty(); })) {
                return;  // se pidió detener el hilo
            }
            id = std::move(pending_.front());
            pending_.pop_front();
            busy_ = true;
        }
        process(id);
        {
            const std::lock_guard lock(mutex_);
            busy_ = false;
        }
        changed_.notify_all();
    }
}

void TranscodeQueue::process(const std::string& id) {
    const fs::path source = store_.sourcePath(id);
    std::error_code ignored;
    const auto sizeBytes = fs::file_size(source, ignored);
    std::optional<double> duration;
    try {
        duration = transcoder_.transcode(source, store_.dashDir(id));
    } catch (const std::exception&) {
        duration.reset();
    }
    if (duration) {
        fs::remove(source, ignored);  // el original ya no se necesita: solo se sirven los segmentos
        metadata_.reportStatus(id, "ready", *duration, ignored ? 0 : sizeBytes);
    } else {
        store_.remove(id);
        metadata_.reportStatus(id, "failed", 0, 0);
    }
}

namespace {

const char* const kIdPattern = "([0-9a-f]{24})";

std::string contentTypeFor(const std::string& file) {
    return file.ends_with(".mpd") ? "application/dash+xml" : "video/iso.segment";
}

void handleUpload(const httplib::Request& req, httplib::Response& res,
                  const httplib::ContentReader& reader, const Config& config,
                  const VideoStore& store, MetadataClient& metadata, TranscodeQueue& queue) {
    const auto claims = authenticate(req, res, config.jwtSecret);
    if (!claims) {
        return;
    }
    const std::string id = req.matches[1];
    const auto video = metadata.getVideo(id, bearerToken(req));
    if (!video) {
        return sendError(res, 404, "video no encontrado");
    }
    if (video->authorId != claims->sub) {
        return sendError(res, 403, "solo el autor puede subir el archivo");
    }
    if (video->status != "pending" && video->status != "failed") {
        return sendError(res, 409, "el video ya fue subido");
    }
    // Se marca antes de leer el body para que una segunda subida simultánea reciba 409.
    metadata.reportStatus(id, "processing", 0, 0);

    const fs::path source = store.sourcePath(id);
    fs::create_directories(source.parent_path());
    std::ofstream out(source, std::ios::binary | std::ios::trunc);
    std::uint64_t received = 0;
    const bool complete = reader([&](const char* data, std::size_t length) {
        received += length;
        if (received > video->sizeBytes) {
            return false;  // excede el tamaño declarado al validar la cuota
        }
        out.write(data, static_cast<std::streamsize>(length));
        return out.good();
    });
    out.close();

    if (!complete || received == 0) {
        store.remove(id);
        metadata.reportStatus(id, "failed", 0, 0);
        if (received > video->sizeBytes) {
            return sendError(res, 413, "el archivo excede el tamaño declarado");
        }
        return sendError(res, 400, "no se pudo recibir el archivo");
    }
    queue.enqueue(id);
    sendJson(res, 202, {{"id", id}, {"status", "processing"}});
}

}  // namespace

void registerRoutes(httplib::Server& server, const Config& config, const VideoStore& store,
                    MetadataClient& metadata, TranscodeQueue& queue) {
    enableCors(server);
    addHealth(server, "streaming-api", config.instanceId);
    server.set_exception_handler(
        [](const httplib::Request&, httplib::Response& res, const std::exception_ptr&) {
            sendError(res, 500, "error interno");
        });

    const std::string videoPath = std::string("/api/videos/") + kIdPattern;

    server.Post(videoPath + "/upload",
                [&config, &store, &metadata, &queue](const httplib::Request& req,
                                                     httplib::Response& res,
                                                     const httplib::ContentReader& reader) {
                    handleUpload(req, res, reader, config, store, metadata, queue);
                });

    // El nombre admite un solo punto y ninguna barra: no se puede salir del directorio.
    server.Get(videoPath + R"(/dash/([A-Za-z0-9_-]+\.(?:mpd|m4s)))",
               [&config, &store, &metadata](const httplib::Request& req, httplib::Response& res) {
                   if (!authenticate(req, res, config.jwtSecret)) {
                       return;
                   }
                   const std::string id = req.matches[1];
                   const std::string file = req.matches[2];
                   const fs::path path = store.dashDir(id) / file;
                   if (!metadata.getVideo(id, bearerToken(req)) || !fs::is_regular_file(path)) {
                       return sendError(res, 404, "video no encontrado");
                   }
                   res.set_file_content(path.string(), contentTypeFor(file));
               });

    server.Delete(std::string("/internal/videos/") + kIdPattern,
                  [&config, &store](const httplib::Request& req, httplib::Response& res) {
                      const auto claims = authenticate(req, res, config.jwtSecret);
                      if (!claims) {
                          return;
                      }
                      if (!claims->isService()) {
                          return sendError(res, 403, "solo para servicios internos");
                      }
                      store.remove(req.matches[1]);
                      res.status = 204;
                  });
}

}  // namespace tecflix::streaming
