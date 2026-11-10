#pragma once

#include <cstdlib>
#include <optional>
#include <string>

#include <httplib.h>
#include <nlohmann/json.hpp>

#include "tecflix/jwt.hpp"

// Convenciones HTTP comunes a todos los servicios (SPEC.md §10).
namespace tecflix {

inline std::string envOr(const char* name, std::string fallback) {
    const char* value = std::getenv(name);
    return (value != nullptr && *value != '\0') ? std::string(value) : fallback;
}

inline void sendJson(httplib::Response& res, int status, const nlohmann::json& body) {
    res.status = status;
    res.set_content(body.dump(), "application/json");
}

inline void sendError(httplib::Response& res, int status, const std::string& message) {
    sendJson(res, status, {{"error", message}});
}

inline std::string bearerToken(const httplib::Request& req) {
    const std::string header = req.get_header_value("Authorization");
    const std::string prefix = "Bearer ";
    return header.starts_with(prefix) ? header.substr(prefix.size()) : std::string{};
}

// Devuelve los claims o responde 401 y devuelve nullopt.
inline std::optional<Claims> authenticate(const httplib::Request& req, httplib::Response& res,
                                          const std::string& secret) {
    auto claims = verifyJwt(bearerToken(req), secret);
    if (!claims) {
        sendError(res, 401, "token inválido o ausente");
    }
    return claims;
}

inline void enableCors(httplib::Server& server) {
    server.set_default_headers({{"Access-Control-Allow-Origin", "*"},
                                {"Access-Control-Allow-Headers", "Authorization, Content-Type, Range"},
                                {"Access-Control-Allow-Methods", "GET, POST, PUT, PATCH, DELETE, OPTIONS"}});
    server.Options(".*", [](const httplib::Request&, httplib::Response& res) { res.status = 204; });
}

inline void addHealth(httplib::Server& server, const std::string& service,
                      const std::string& instance) {
    server.Get("/health", [service, instance](const httplib::Request&, httplib::Response& res) {
        sendJson(res, 200, {{"status", "ok"}, {"service", service}, {"instance", instance}});
    });
}

}  // namespace tecflix
