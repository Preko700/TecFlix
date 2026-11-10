#include <iostream>

#include "streaming.hpp"
#include "tecflix/http.hpp"

int main() {
    using namespace tecflix;
    using namespace tecflix::streaming;

    const Config config{envOr("TECFLIX_JWT_SECRET", ""), envOr("INSTANCE_ID", "streaming-1")};
    if (config.jwtSecret.empty()) {
        std::cerr << "Falta la variable TECFLIX_JWT_SECRET\n";
        return 1;
    }
    const int port = std::stoi(envOr("PORT", "8083"));

    const VideoStore store(envOr("DATA_DIR", "./data"));
    HttpMetadataClient metadata(envOr("METADATA_URL", "http://localhost:8082"), config.jwtSecret);
    FfmpegTranscoder transcoder;
    TranscodeQueue queue(store, transcoder, metadata);

    httplib::Server server;
    registerRoutes(server, config, store, metadata, queue);

    std::cout << "streaming-api (" << config.instanceId << ") escuchando en :" << port << '\n';
    if (!server.listen("0.0.0.0", port)) {
        std::cerr << "No se pudo abrir el puerto " << port << '\n';
        return 1;
    }
    return 0;
}
