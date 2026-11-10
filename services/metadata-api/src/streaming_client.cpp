#include "metadata.hpp"

namespace tecflix::metadata {

HttpStreamingClient::HttpStreamingClient(std::string baseUrl, std::string jwtSecret)
    : baseUrl_(std::move(baseUrl)), jwtSecret_(std::move(jwtSecret)) {}

bool HttpStreamingClient::deleteFiles(const std::string& videoId) {
    httplib::Client client(baseUrl_);
    const auto response = client.Delete(
        "/internal/videos/" + videoId,
        httplib::Headers{{"Authorization", "Bearer " + serviceToken("metadata-api", jwtSecret_)}});
    return response && response->status == 204;
}

}  // namespace tecflix::metadata
