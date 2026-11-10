#include <gtest/gtest.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>

#include "tecflix/http.hpp"
#include "tecflix/jwt.hpp"

using namespace tecflix;

namespace {
const std::string kSecret = "secreto-de-prueba";
constexpr std::int64_t kNow = 1'000'000;

Claims sampleClaims() {
    Claims claims;
    claims.sub = "42";
    claims.username = "adrian";
    claims.quota = 1024;
    claims.exp = kNow + 60;
    return claims;
}
}  // namespace

TEST(Base64Url, RoundTripsEveryPaddingLength) {
    const std::vector<std::string> samples = {"", "a", "ab", "abc", "abcd",
                                              std::string("\x00\xff\x10", 3)};
    for (const std::string& text : samples) {
        EXPECT_EQ(base64UrlDecode(base64UrlEncode(text)), text);
    }
}

TEST(Base64Url, MatchesKnownVector) {
    EXPECT_EQ(base64UrlEncode("hello?>"), "aGVsbG8_Pg");
}

TEST(Base64Url, RejectsInvalidCharacters) {
    EXPECT_FALSE(base64UrlDecode("ab+c"));
}

TEST(Jwt, UsesStandardHs256Header) {
    const std::string token = signJwt(sampleClaims(), kSecret);
    EXPECT_EQ(token.substr(0, token.find('.')), "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9");
}

TEST(Jwt, SignThenVerifyReturnsSameClaims) {
    const auto verified = verifyJwt(signJwt(sampleClaims(), kSecret), kSecret, kNow);
    ASSERT_TRUE(verified);
    EXPECT_EQ(verified->sub, "42");
    EXPECT_EQ(verified->username, "adrian");
    EXPECT_EQ(verified->role, "user");
    EXPECT_EQ(verified->quota, 1024u);
    EXPECT_FALSE(verified->isService());
}

TEST(Jwt, RejectsWrongSecret) {
    EXPECT_FALSE(verifyJwt(signJwt(sampleClaims(), kSecret), "otro", kNow));
}

TEST(Jwt, RejectsExpired) {
    EXPECT_FALSE(verifyJwt(signJwt(sampleClaims(), kSecret), kSecret, kNow + 60));
}

TEST(Jwt, RejectsTamperedPayload) {
    std::string token = signJwt(sampleClaims(), kSecret);
    const auto dot = token.find('.');
    token[dot + 3] = token[dot + 3] == 'A' ? 'B' : 'A';
    EXPECT_FALSE(verifyJwt(token, kSecret, kNow));
}

TEST(Jwt, RejectsMalformedTokens) {
    EXPECT_FALSE(verifyJwt("", kSecret, kNow));
    EXPECT_FALSE(verifyJwt("sin-puntos", kSecret, kNow));
    EXPECT_FALSE(verifyJwt("solo.uno", kSecret, kNow));
}

TEST(Jwt, RejectsValidSignatureOverBadPayload) {
    // Token bien firmado cuyo payload no son claims válidos.
    const auto forge = [](const std::string& encodedPayload) {
        const std::string signedPart = "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9." + encodedPayload;
        unsigned char digest[EVP_MAX_MD_SIZE];
        unsigned int length = 0;
        HMAC(EVP_sha256(), kSecret.data(), static_cast<int>(kSecret.size()),
             reinterpret_cast<const unsigned char*>(signedPart.data()), signedPart.size(), digest,
             &length);
        return signedPart + "." +
               base64UrlEncode({reinterpret_cast<const char*>(digest), length});
    };
    EXPECT_FALSE(verifyJwt(forge("***"), kSecret, kNow));                         // no es base64url
    EXPECT_FALSE(verifyJwt(forge(base64UrlEncode("[1,2]")), kSecret, kNow));      // no es objeto
    EXPECT_FALSE(verifyJwt(forge(base64UrlEncode(R"({"sub":"1"})")), kSecret, kNow));  // sin exp
    EXPECT_TRUE(verifyJwt(forge(base64UrlEncode(R"({"sub":"1","exp":1000001})")), kSecret, kNow));
}

TEST(Http, BearerTokenExtractsOnlyBearerScheme) {
    httplib::Request req;
    req.set_header("Authorization", "Bearer abc");
    EXPECT_EQ(bearerToken(req), "abc");
    httplib::Request basic;
    basic.set_header("Authorization", "Basic abc");
    EXPECT_EQ(bearerToken(basic), "");
}

TEST(Http, AuthenticateAnswers401WithoutToken) {
    httplib::Request req;
    httplib::Response res;
    EXPECT_FALSE(authenticate(req, res, kSecret));
    EXPECT_EQ(res.status, 401);
    EXPECT_EQ(nlohmann::json::parse(res.body).at("error"), "token inválido o ausente");
}

TEST(Http, AuthenticateAcceptsValidToken) {
    Claims claims = sampleClaims();
    claims.exp = std::time(nullptr) + 60;
    httplib::Request req;
    req.set_header("Authorization", "Bearer " + signJwt(claims, kSecret));
    httplib::Response res;
    const auto verified = authenticate(req, res, kSecret);
    ASSERT_TRUE(verified);
    EXPECT_EQ(verified->sub, "42");
}

TEST(Http, EnvOrFallsBackWhenUnset) {
    EXPECT_EQ(envOr("TECFLIX_VARIABLE_INEXISTENTE", "x"), "x");
}
