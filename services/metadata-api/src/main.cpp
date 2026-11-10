#include <iostream>

#include "metadata.hpp"
#include "tecflix/http.hpp"

int main() {
    using namespace tecflix;
    using namespace tecflix::metadata;

    Config config{envOr("TECFLIX_JWT_SECRET", ""), envOr("INSTANCE_ID", "metadata-1")};
    if (config.jwtSecret.empty()) {
        std::cerr << "Falta la variable TECFLIX_JWT_SECRET\n";
        return 1;
    }
    const int port = std::stoi(envOr("PORT", "8082"));

    MongoVideoRepository repository(envOr("MONGO_URI", "mongodb://localhost:27017"),
                                    envOr("MONGO_DB", "tecflix"));
    HttpStreamingClient streaming(envOr("STREAMING_URL", "http://localhost:8083"),
                                  config.jwtSecret);
    MetadataApi api(config, repository, streaming);

    httplib::Server server;
    api.registerRoutes(server);

    std::cout << "metadata-api (" << config.instanceId << ") escuchando en :" << port << '\n';
    if (!server.listen("0.0.0.0", port)) {
        std::cerr << "No se pudo abrir el puerto " << port << '\n';
        return 1;
    }
    return 0;
}
