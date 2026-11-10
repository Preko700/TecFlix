#include <algorithm>
#include <charconv>
#include <ctime>
#include <map>
#include <set>

#include "metadata.hpp"
#include "tecflix/http.hpp"

namespace tecflix::metadata {

namespace {

using nlohmann::json;

const std::set<std::string> kStatuses = {"pending", "processing", "ready", "failed"};
// Valor de ?sort= → campo del documento.
const std::map<std::string, std::string> kSortFields = {
    {"title", "title"}, {"author", "authorName"}, {"uploadedAt", "uploadedAt"}};
constexpr int kMaxPageSize = 100;

std::optional<json> parseBody(const httplib::Request& req, httplib::Response& res) {
    json body = json::parse(req.body, nullptr, false);
    if (!body.is_object()) {
        sendError(res, 400, "se esperaba un objeto JSON");
        return std::nullopt;
    }
    return body;
}

// Copia a `changes` los campos editables presentes en `body`.
// Devuelve el mensaje de error si alguno es inválido.
std::optional<std::string> readEditableFields(const json& body, json& changes) {
    for (const char* field : {"title", "description", "topic"}) {
        if (!body.contains(field)) {
            continue;
        }
        if (!body[field].is_string()) {
            return std::string(field) + " debe ser texto";
        }
        changes[field] = body[field];
    }
    if (changes.contains("title") && changes["title"].get_ref<const std::string&>().empty()) {
        return "title no puede estar vacío";
    }
    if (body.contains("tags")) {
        const json& tags = body["tags"];
        if (!tags.is_array() ||
            !std::all_of(tags.begin(), tags.end(), [](const json& tag) { return tag.is_string(); })) {
            return "tags debe ser una lista de textos";
        }
        changes["tags"] = tags;
    }
    if (body.contains("visibility")) {
        if (body["visibility"] != "public" && body["visibility"] != "private") {
            return "visibility debe ser \"public\" o \"private\"";
        }
        changes["visibility"] = body["visibility"];
    }
    return std::nullopt;
}

// Parámetro entero de la URL; `fallback` si no viene, nullopt si no es un entero.
std::optional<int> intParam(const httplib::Request& req, const char* name, int fallback) {
    if (!req.has_param(name)) {
        return fallback;
    }
    const std::string text = req.get_param_value(name);
    int value = 0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc{} || end != text.data() + text.size()) {
        return std::nullopt;
    }
    return value;
}

}  // namespace

MetadataApi::MetadataApi(Config config, VideoRepository& repository, StreamingClient& streaming)
    : config_(std::move(config)), repository_(repository), streaming_(streaming) {}

httplib::Server::Handler MetadataApi::secured(Handler handler, bool serviceOnly) {
    return [this, handler, serviceOnly](const httplib::Request& req, httplib::Response& res) {
        const auto claims = authenticate(req, res, config_.jwtSecret);
        if (!claims) {
            return;
        }
        if (serviceOnly && !claims->isService()) {
            return sendError(res, 403, "solo para servicios internos");
        }
        (this->*handler)(req, res, *claims);
    };
}

void MetadataApi::registerRoutes(httplib::Server& server) {
    enableCors(server);
    addHealth(server, "metadata-api", config_.instanceId);
    server.set_exception_handler(
        [](const httplib::Request&, httplib::Response& res, const std::exception_ptr&) {
            sendError(res, 500, "error interno");
        });

    const std::string id = "([0-9a-f]{24})";
    server.Get("/api/videos", secured(&MetadataApi::listVideos));
    server.Post("/api/videos", secured(&MetadataApi::createVideo));
    server.Get("/api/videos/" + id, secured(&MetadataApi::getVideo));
    server.Patch("/api/videos/" + id, secured(&MetadataApi::patchVideo));
    server.Delete("/api/videos/" + id, secured(&MetadataApi::deleteVideo));
    server.Get(R"(/api/users/([^/]+)/usage)", secured(&MetadataApi::getUsage));
    server.Patch("/internal/videos/" + id, secured(&MetadataApi::patchStatus, true));
    server.Delete(R"(/internal/users/([^/]+)/videos)",
                  secured(&MetadataApi::deleteUserVideos, true));
}

std::optional<Video> MetadataApi::findVisible(const std::string& id, const Claims& claims,
                                              httplib::Response& res) {
    auto video = repository_.find(id);
    const bool visible = video && (claims.isService() || video->authorId == claims.sub ||
                                   video->visibility == "public");
    if (!visible) {
        // Mismo 404 para "no existe" y "es privado": no se revela que el video existe.
        sendError(res, 404, "video no encontrado");
        return std::nullopt;
    }
    return video;
}

std::optional<Video> MetadataApi::findOwned(const std::string& id, const Claims& claims,
                                            httplib::Response& res) {
    auto video = findVisible(id, claims, res);
    if (video && video->authorId != claims.sub) {
        sendError(res, 403, "solo el autor puede modificar el video");
        return std::nullopt;
    }
    return video;
}

void MetadataApi::listVideos(const httplib::Request& req, httplib::Response& res,
                             const Claims& claims) {
    ListQuery query;
    query.viewerId = claims.sub;

    const auto page = intParam(req, "page", query.page);
    const auto pageSize = intParam(req, "pageSize", query.pageSize);
    if (!page || *page < 1 || !pageSize || *pageSize < 1 || *pageSize > kMaxPageSize) {
        return sendError(res, 400, "page debe ser >= 1 y pageSize estar entre 1 y 100");
    }
    query.page = *page;
    query.pageSize = *pageSize;

    if (req.has_param("sort")) {
        const auto field = kSortFields.find(req.get_param_value("sort"));
        if (field == kSortFields.end()) {
            return sendError(res, 400, "sort debe ser title, author o uploadedAt");
        }
        query.sortField = field->second;
        query.ascending = true;  // sin ?order, un orden explícito es ascendente
    }
    if (req.has_param("order")) {
        const std::string order = req.get_param_value("order");
        if (order != "asc" && order != "desc") {
            return sendError(res, 400, "order debe ser asc o desc");
        }
        query.ascending = order == "asc";
    }

    const VideoPage result = repository_.list(query);
    sendJson(res, 200,
             {{"items", result.items},
              {"total", result.total},
              {"page", query.page},
              {"pageSize", query.pageSize}});
}

void MetadataApi::createVideo(const httplib::Request& req, httplib::Response& res,
                              const Claims& claims) {
    const auto body = parseBody(req, res);
    if (!body) {
        return;
    }
    json fields;
    if (const auto error = readEditableFields(*body, fields)) {
        return sendError(res, 400, *error);
    }
    if (!fields.contains("title") || !fields.contains("visibility")) {
        return sendError(res, 400, "title y visibility son obligatorios");
    }
    if (!body->contains("sizeBytes") || !(*body)["sizeBytes"].is_number_unsigned() ||
        (*body)["sizeBytes"] == 0) {
        return sendError(res, 400, "sizeBytes debe ser un entero positivo");
    }

    Video video = fields.get<Video>();
    video.sizeBytes = (*body)["sizeBytes"].get<std::uint64_t>();
    // shortcut: consultar y luego insertar no es atómico; dos creaciones simultáneas del mismo
    // usuario podrían pasarse de la cuota. Aceptable en V1.0.
    const std::uint64_t used = repository_.usedBytes(claims.sub);
    if (video.sizeBytes > claims.quota || used > claims.quota - video.sizeBytes) {
        return sendError(res, 409, "cuota de almacenamiento excedida");
    }
    video.authorId = claims.sub;
    video.authorName = claims.username;
    video.uploadedAt = std::time(nullptr);
    sendJson(res, 201, repository_.insert(std::move(video)));
}

void MetadataApi::getVideo(const httplib::Request& req, httplib::Response& res,
                           const Claims& claims) {
    if (const auto video = findVisible(req.matches[1], claims, res)) {
        sendJson(res, 200, *video);
    }
}

void MetadataApi::patchVideo(const httplib::Request& req, httplib::Response& res,
                             const Claims& claims) {
    const std::string id = req.matches[1];
    const auto body = parseBody(req, res);
    if (!body || !findOwned(id, claims, res)) {
        return;
    }
    json changes = json::object();
    if (const auto error = readEditableFields(*body, changes)) {
        return sendError(res, 400, *error);
    }
    if (changes.empty()) {
        return sendError(res, 400, "no hay campos para actualizar");
    }
    if (const auto updated = repository_.update(id, changes)) {
        return sendJson(res, 200, *updated);
    }
    sendError(res, 404, "video no encontrado");
}

void MetadataApi::deleteVideo(const httplib::Request& req, httplib::Response& res,
                              const Claims& claims) {
    const std::string id = req.matches[1];
    if (!findOwned(id, claims, res)) {
        return;
    }
    repository_.remove(id);
    // shortcut: si streaming está caído los archivos quedan huérfanos; en V2.0 se reintenta.
    streaming_.deleteFiles(id);
    res.status = 204;
}

void MetadataApi::getUsage(const httplib::Request& req, httplib::Response& res,
                           const Claims& claims) {
    const std::string userId = req.matches[1];
    if (!claims.isService() && claims.sub != userId) {
        return sendError(res, 403, "solo se puede consultar el uso propio");
    }
    sendJson(res, 200, {{"usedBytes", repository_.usedBytes(userId)}});
}

void MetadataApi::patchStatus(const httplib::Request& req, httplib::Response& res,
                              const Claims&) {
    const auto body = parseBody(req, res);
    if (!body) {
        return;
    }
    if (!body->contains("status") || !(*body)["status"].is_string() ||
        !kStatuses.contains((*body)["status"].get<std::string>())) {
        return sendError(res, 400, "status inválido");
    }
    json changes = {{"status", (*body)["status"]}};
    if (body->contains("durationSeconds")) {
        if (!(*body)["durationSeconds"].is_number()) {
            return sendError(res, 400, "durationSeconds debe ser un número");
        }
        changes["durationSeconds"] = (*body)["durationSeconds"].get<double>();
    }
    if (body->contains("sizeBytes")) {
        if (!(*body)["sizeBytes"].is_number_unsigned()) {
            return sendError(res, 400, "sizeBytes debe ser un entero no negativo");
        }
        changes["sizeBytes"] = (*body)["sizeBytes"];
    }
    if (const auto updated = repository_.update(req.matches[1], changes)) {
        return sendJson(res, 200, *updated);
    }
    sendError(res, 404, "video no encontrado");
}

void MetadataApi::deleteUserVideos(const httplib::Request& req, httplib::Response& res,
                                   const Claims&) {
    for (const std::string& id : repository_.removeByAuthor(req.matches[1])) {
        streaming_.deleteFiles(id);
    }
    res.status = 204;
}

}  // namespace tecflix::metadata
