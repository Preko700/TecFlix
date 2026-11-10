#include <nlohmann/json.hpp>

#include "streaming.hpp"
#include "tecflix/jwt.hpp"

namespace tecflix::streaming {

HttpMetadataClient::HttpMetadataClient(std::string baseUrl, std::string jwtSecret)
    : baseUrl_(std::move(baseUrl)), jwtSecret_(std::move(jwtSecret)) {}

// shortcut: una conexión nueva por llamada; candidato de optimización V2.0 (keep-alive / caché).
std::optional<VideoInfo> HttpMetadataClient::getVideo(const std::string& id,
                                                      const std::string& userToken) {
    httplib::Client client(baseUrl_);
    const auto response =
        client.Get("/api/videos/" + id, {{"Authorization", "Bearer " + userToken}});
    if (!response || response->status != 200) {
        return std::nullopt;
    }
    const auto body = nlohmann::json::parse(response->body, nullptr, false);
    if (!body.is_object() || !body.contains("authorId") || !body.contains("status")) {
        return std::nullopt;
    }
    return VideoInfo{body.value("authorId", ""), body.value("status", ""),
                     body.value("sizeBytes", std::uint64_t{0})};
}

bool HttpMetadataClient::reportStatus(const std::string& id, const std::string& status,
                                      double durationSeconds, std::uint64_t sizeBytes) {
    Claims service;
    service.sub = "streaming-api";
    service.role = "service";
    service.exp = std::time(nullptr) + 60;

    nlohmann::json body = {{"status", status}};
    if (status == "ready") {
        body["durationSeconds"] = durationSeconds;
        body["sizeBytes"] = sizeBytes;
    }
    httplib::Client client(baseUrl_);
    const auto response =
        client.Patch("/internal/videos/" + id,
                     {{"Authorization", "Bearer " + signJwt(service, jwtSecret_)}}, body.dump(),
                     "application/json");
    return response && response->status == 200;
}

}  // namespace tecflix::streaming
