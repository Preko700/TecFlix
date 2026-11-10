#include "tecflix/jwt.hpp"

#include <array>

#include <nlohmann/json.hpp>
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>

namespace tecflix {

namespace {

constexpr std::string_view kAlphabet =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
constexpr std::string_view kHeader = R"({"alg":"HS256","typ":"JWT"})";

std::string hmacSha256(std::string_view key, std::string_view data) {
    std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
    unsigned int length = 0;
    HMAC(EVP_sha256(), key.data(), static_cast<int>(key.size()),
         reinterpret_cast<const unsigned char*>(data.data()), data.size(), digest.data(), &length);
    return {reinterpret_cast<const char*>(digest.data()), length};
}

}  // namespace

std::string base64UrlEncode(std::string_view data) {
    std::string out;
    out.reserve((data.size() * 4 + 2) / 3);
    std::uint32_t buffer = 0;
    int bits = 0;
    for (unsigned char byte : data) {
        buffer = (buffer << 8) | byte;
        bits += 8;
        while (bits >= 6) {
            bits -= 6;
            out.push_back(kAlphabet[(buffer >> bits) & 0x3F]);
        }
    }
    if (bits > 0) {
        out.push_back(kAlphabet[(buffer << (6 - bits)) & 0x3F]);
    }
    return out;
}

std::optional<std::string> base64UrlDecode(std::string_view text) {
    std::string out;
    out.reserve(text.size() * 3 / 4);
    std::uint32_t buffer = 0;
    int bits = 0;
    for (char symbol : text) {
        const auto index = kAlphabet.find(symbol);
        if (index == std::string_view::npos) {
            return std::nullopt;
        }
        buffer = (buffer << 6) | static_cast<std::uint32_t>(index);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<char>((buffer >> bits) & 0xFF));
        }
    }
    return out;
}

std::string signJwt(const Claims& claims, std::string_view secret) {
    const nlohmann::json payload = {{"sub", claims.sub},
                                    {"username", claims.username},
                                    {"role", claims.role},
                                    {"quota", claims.quota},
                                    {"exp", claims.exp}};
    const std::string signedPart =
        base64UrlEncode(kHeader) + "." + base64UrlEncode(payload.dump());
    return signedPart + "." + base64UrlEncode(hmacSha256(secret, signedPart));
}

std::string serviceToken(const std::string& serviceName, std::string_view secret) {
    Claims claims;
    claims.sub = serviceName;
    claims.role = "service";
    claims.exp = std::time(nullptr) + 60;
    return signJwt(claims, secret);
}

std::optional<Claims> verifyJwt(std::string_view token, std::string_view secret,
                                std::int64_t now) {
    const auto firstDot = token.find('.');
    const auto lastDot = token.rfind('.');
    if (firstDot == std::string_view::npos || firstDot == lastDot) {
        return std::nullopt;
    }
    const std::string_view signedPart = token.substr(0, lastDot);
    const std::string expected = base64UrlEncode(hmacSha256(secret, signedPart));
    const std::string_view signature = token.substr(lastDot + 1);
    // Comparación en tiempo constante: no filtra cuántos caracteres coinciden.
    if (signature.size() != expected.size() ||
        CRYPTO_memcmp(signature.data(), expected.data(), expected.size()) != 0) {
        return std::nullopt;
    }
    const auto payloadText = base64UrlDecode(token.substr(firstDot + 1, lastDot - firstDot - 1));
    if (!payloadText) {
        return std::nullopt;
    }
    const auto payload = nlohmann::json::parse(*payloadText, nullptr, false);
    if (!payload.is_object()) {
        return std::nullopt;
    }
    Claims claims;
    try {
        claims.sub = payload.at("sub").get<std::string>();
        claims.exp = payload.at("exp").get<std::int64_t>();
        claims.username = payload.value("username", "");
        claims.role = payload.value("role", "user");
        claims.quota = payload.value("quota", std::uint64_t{0});
    } catch (const nlohmann::json::exception&) {
        return std::nullopt;
    }
    if (claims.exp <= now) {
        return std::nullopt;
    }
    return claims;
}

}  // namespace tecflix
