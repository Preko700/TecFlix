#pragma once

#include <cstdint>
#include <ctime>
#include <optional>
#include <string>
#include <string_view>

namespace tecflix {

// Claims del JWT compartido por todos los servicios (SPEC.md §10.1).
struct Claims {
    std::string sub;
    std::string username;
    std::string role = "user";
    std::uint64_t quota = 0;
    std::int64_t exp = 0;

    bool isService() const { return role == "service"; }
};

std::string base64UrlEncode(std::string_view data);
std::optional<std::string> base64UrlDecode(std::string_view text);

std::string signJwt(const Claims& claims, std::string_view secret);

// nullopt si el formato o la firma son inválidos, o si el token expiró.
std::optional<Claims> verifyJwt(std::string_view token, std::string_view secret,
                                std::int64_t now = std::time(nullptr));

}  // namespace tecflix
