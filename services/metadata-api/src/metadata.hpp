#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <httplib.h>
#include <nlohmann/json.hpp>

#include "tecflix/jwt.hpp"

using mongoc_client_pool_t = struct _mongoc_client_pool_t;

// API de metadata (SPEC.md §4.3 y §10.3).
namespace tecflix::metadata {

struct Video {
    std::string id;  // ObjectId de Mongo, 24 caracteres hexadecimales
    std::string title;
    std::string description;
    std::string topic;
    std::vector<std::string> tags;
    std::string visibility = "private";  // "public" | "private"
    std::string status = "pending";      // "pending" | "processing" | "ready" | "failed"
    std::string authorId;
    std::string authorName;
    std::uint64_t sizeBytes = 0;
    double durationSeconds = 0;
    std::int64_t uploadedAt = 0;  // epoch, segundos
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(Video, id, title, description, topic, tags,
                                                visibility, status, authorId, authorName,
                                                sizeBytes, durationSeconds, uploadedAt)

struct ListQuery {
    std::string viewerId;                  // ve sus videos + los públicos listos de otros
    std::string sortField = "uploadedAt";  // "title" | "authorName" | "uploadedAt"
    bool ascending = false;
    int page = 1;
    int pageSize = 20;
};

struct VideoPage {
    std::vector<Video> items;
    std::uint64_t total = 0;
};

// Persistencia de videos. Interfaz para poder probar las rutas sin MongoDB.
class VideoRepository {
public:
    virtual ~VideoRepository() = default;
    virtual Video insert(Video video) = 0;  // devuelve el video con su id asignado
    virtual std::optional<Video> find(const std::string& id) = 0;
    // Aplica solo los campos presentes en `changes`; nullopt si el video no existe.
    virtual std::optional<Video> update(const std::string& id, const nlohmann::json& changes) = 0;
    virtual bool remove(const std::string& id) = 0;
    virtual VideoPage list(const ListQuery& query) = 0;
    // Bytes que cuentan contra la cuota (todo menos los videos en "failed").
    virtual std::uint64_t usedBytes(const std::string& authorId) = 0;
    virtual std::vector<std::string> removeByAuthor(const std::string& authorId) = 0;  // ids
};

class MongoVideoRepository : public VideoRepository {
public:
    MongoVideoRepository(const std::string& uri, std::string database);
    ~MongoVideoRepository() override;
    MongoVideoRepository(const MongoVideoRepository&) = delete;
    MongoVideoRepository& operator=(const MongoVideoRepository&) = delete;

    Video insert(Video video) override;
    std::optional<Video> find(const std::string& id) override;
    std::optional<Video> update(const std::string& id, const nlohmann::json& changes) override;
    bool remove(const std::string& id) override;
    VideoPage list(const ListQuery& query) override;
    std::uint64_t usedBytes(const std::string& authorId) override;
    std::vector<std::string> removeByAuthor(const std::string& authorId) override;

private:
    mongoc_client_pool_t* pool_;  // un cliente por hilo: los clientes de mongoc no son thread-safe
    std::string database_;
};

// Lo que metadata necesita del API de streaming.
class StreamingClient {
public:
    virtual ~StreamingClient() = default;
    virtual bool deleteFiles(const std::string& videoId) = 0;
};

class HttpStreamingClient : public StreamingClient {
public:
    HttpStreamingClient(std::string baseUrl, std::string jwtSecret);
    bool deleteFiles(const std::string& videoId) override;

private:
    std::string baseUrl_;
    std::string jwtSecret_;
};

struct Config {
    std::string jwtSecret;
    std::string instanceId;
};

// Rutas HTTP. Debe vivir más que el servidor donde se registra.
class MetadataApi {
public:
    MetadataApi(Config config, VideoRepository& repository, StreamingClient& streaming);
    void registerRoutes(httplib::Server& server);

private:
    using Handler = void (MetadataApi::*)(const httplib::Request&, httplib::Response&,
                                          const Claims&);
    // Envuelve un handler: valida el JWT y, si se pide, exige rol "service".
    httplib::Server::Handler secured(Handler handler, bool serviceOnly = false);
    // El video si existe y `claims` puede verlo; si no, responde 404.
    std::optional<Video> findVisible(const std::string& id, const Claims& claims,
                                     httplib::Response& res);
    // Igual que findVisible, pero además exige ser el autor (403 si no).
    std::optional<Video> findOwned(const std::string& id, const Claims& claims,
                                   httplib::Response& res);

    void listVideos(const httplib::Request& req, httplib::Response& res, const Claims& claims);
    void createVideo(const httplib::Request& req, httplib::Response& res, const Claims& claims);
    void getVideo(const httplib::Request& req, httplib::Response& res, const Claims& claims);
    void patchVideo(const httplib::Request& req, httplib::Response& res, const Claims& claims);
    void deleteVideo(const httplib::Request& req, httplib::Response& res, const Claims& claims);
    void getUsage(const httplib::Request& req, httplib::Response& res, const Claims& claims);
    void patchStatus(const httplib::Request& req, httplib::Response& res, const Claims& claims);
    void deleteUserVideos(const httplib::Request& req, httplib::Response& res,
                          const Claims& claims);

    Config config_;
    VideoRepository& repository_;
    StreamingClient& streaming_;
};

}  // namespace tecflix::metadata
