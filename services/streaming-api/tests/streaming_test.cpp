#include <cstdlib>
#include <fstream>
#include <map>
#include <thread>
#include <vector>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "streaming.hpp"
#include "tecflix/http.hpp"

using namespace tecflix;
using namespace tecflix::streaming;

namespace {

const std::string kSecret = "secreto-de-prueba";
const std::string kVideoId = "0123456789abcdef01234567";
const std::string kUploadPath = "/api/videos/" + kVideoId + "/upload";
const std::string kManifestPath = "/api/videos/" + kVideoId + "/dash/manifest.mpd";

std::string tokenFor(const std::string& userId, const std::string& role = "user") {
    Claims claims;
    claims.sub = userId;
    claims.role = role;
    claims.exp = std::time(nullptr) + 60;
    return signJwt(claims, kSecret);
}

httplib::Headers auth(const std::string& userId, const std::string& role = "user") {
    return {{"Authorization", "Bearer " + tokenFor(userId, role)}};
}

fs::path freshTempDir(const std::string& name) {
    const fs::path dir = fs::temp_directory_path() / ("tecflix-" + name + "-" +
                                                      std::to_string(::getpid()));
    fs::remove_all(dir);
    fs::create_directories(dir);
    return dir;
}

class FakeMetadata : public MetadataClient {
public:
    std::optional<VideoInfo> getVideo(const std::string& id, const std::string&) override {
        const auto found = videos.find(id);
        return found == videos.end() ? std::nullopt : std::optional<VideoInfo>(found->second);
    }
    bool reportStatus(const std::string& id, const std::string& status, double durationSeconds,
                      std::uint64_t sizeBytes) override {
        statuses.push_back(status);
        lastDuration = durationSeconds;
        lastSize = sizeBytes;
        if (videos.contains(id)) {
            videos[id].status = status;
        }
        return true;
    }
    std::map<std::string, VideoInfo> videos;
    std::vector<std::string> statuses;
    double lastDuration = 0;
    std::uint64_t lastSize = 0;
};

class FakeTranscoder : public Transcoder {
public:
    std::optional<double> transcode(const fs::path&, const fs::path& outputDir) override {
        if (throws) {
            throw std::runtime_error("ffmpeg explotó");
        }
        if (!succeeds) {
            return std::nullopt;
        }
        fs::create_directories(outputDir);
        std::ofstream(outputDir / "manifest.mpd") << "<MPD/>";
        std::ofstream(outputDir / "chunk-0-00001.m4s") << "segmento";
        return 12.5;
    }
    bool succeeds = true;
    bool throws = false;
};

// Levanta el servidor real en un puerto libre con metadata y transcoder falsos.
class StreamingApiTest : public ::testing::Test {
protected:
    void SetUp() override {
        registerRoutes(server, {kSecret, "test-1"}, store, metadata, queue);
        port = server.bind_to_any_port("127.0.0.1");
        listener = std::thread([this] { server.listen_after_bind(); });
        server.wait_until_ready();
        metadata.videos[kVideoId] = {"user-1", "pending", 100};
    }
    void TearDown() override {
        server.stop();
        listener.join();
        fs::remove_all(root);
    }
    httplib::Client client() const { return httplib::Client("127.0.0.1", port); }
    httplib::Result upload(const std::string& body, const std::string& userId = "user-1") {
        return client().Post(kUploadPath, auth(userId), body, "video/mp4");
    }

    fs::path root = freshTempDir("api");
    VideoStore store{root};
    FakeMetadata metadata;
    FakeTranscoder transcoder;
    TranscodeQueue queue{store, transcoder, metadata};
    httplib::Server server;
    int port = 0;
    std::thread listener;
};

}  // namespace

TEST_F(StreamingApiTest, HealthReportsServiceAndInstance) {
    const auto res = client().Get("/health");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 200);
    const auto body = nlohmann::json::parse(res->body);
    EXPECT_EQ(body.at("service"), "streaming-api");
    EXPECT_EQ(body.at("instance"), "test-1");
}

TEST_F(StreamingApiTest, PreflightAllowsCrossOriginRequests) {
    const auto res = client().Options(kUploadPath);
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 204);
    EXPECT_EQ(res->get_header_value("Access-Control-Allow-Origin"), "*");
}

TEST_F(StreamingApiTest, UploadRequiresToken) {
    const auto res = client().Post(kUploadPath, "datos", "video/mp4");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 401);
}

TEST_F(StreamingApiTest, UploadOfUnknownVideoIs404) {
    metadata.videos.clear();
    EXPECT_EQ(upload("datos")->status, 404);
}

TEST_F(StreamingApiTest, UploadByNonAuthorIs403) {
    EXPECT_EQ(upload("datos", "user-2")->status, 403);
}

TEST_F(StreamingApiTest, UploadTwiceIs409) {
    metadata.videos[kVideoId].status = "ready";
    EXPECT_EQ(upload("datos")->status, 409);
}

TEST_F(StreamingApiTest, UploadLargerThanDeclaredSizeIs413AndLeavesNoFiles) {
    EXPECT_EQ(upload(std::string(101, 'x'))->status, 413);
    EXPECT_FALSE(fs::exists(store.sourcePath(kVideoId)));
    EXPECT_EQ(metadata.statuses.back(), "failed");
}

TEST_F(StreamingApiTest, EmptyUploadIs400) {
    EXPECT_EQ(upload("")->status, 400);
    EXPECT_EQ(metadata.statuses.back(), "failed");
}

TEST_F(StreamingApiTest, UploadTranscodesAndServesDash) {
    const auto res = upload(std::string(100, 'x'));
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 202);
    queue.waitIdle();

    EXPECT_EQ(metadata.statuses, (std::vector<std::string>{"processing", "ready"}));
    EXPECT_DOUBLE_EQ(metadata.lastDuration, 12.5);
    EXPECT_EQ(metadata.lastSize, 100u);
    EXPECT_FALSE(fs::exists(store.sourcePath(kVideoId))) << "el original se borra al terminar";

    // Cualquier usuario al que metadata le deje ver el video puede reproducirlo.
    const auto manifest = client().Get(kManifestPath, auth("user-2"));
    ASSERT_TRUE(manifest);
    EXPECT_EQ(manifest->status, 200);
    EXPECT_EQ(manifest->body, "<MPD/>");
    EXPECT_EQ(manifest->get_header_value("Content-Type"), "application/dash+xml");

    const auto segment =
        client().Get("/api/videos/" + kVideoId + "/dash/chunk-0-00001.m4s", auth("user-2"));
    EXPECT_EQ(segment->status, 200);
    EXPECT_EQ(segment->get_header_value("Content-Type"), "video/iso.segment");
}

TEST_F(StreamingApiTest, FailedTranscodeIsReportedAndCanBeRetried) {
    transcoder.succeeds = false;
    EXPECT_EQ(upload("no-es-video")->status, 202);
    queue.waitIdle();
    EXPECT_EQ(metadata.statuses.back(), "failed");
    EXPECT_FALSE(fs::exists(store.dashDir(kVideoId)));

    transcoder.succeeds = true;
    EXPECT_EQ(upload("ahora-si")->status, 202);
    queue.waitIdle();
    EXPECT_EQ(metadata.statuses.back(), "ready");
}

TEST_F(StreamingApiTest, TranscoderExceptionIsReportedAsFailed) {
    transcoder.throws = true;
    EXPECT_EQ(upload("datos")->status, 202);
    queue.waitIdle();
    EXPECT_EQ(metadata.statuses.back(), "failed");
}

TEST_F(StreamingApiTest, PlaybackRequiresTokenAndVisibility) {
    upload("datos");
    queue.waitIdle();
    EXPECT_EQ(client().Get(kManifestPath)->status, 401);

    metadata.videos.clear();  // metadata responde 404: privado de otro usuario o borrado
    EXPECT_EQ(client().Get(kManifestPath, auth("user-2"))->status, 404);
}

TEST_F(StreamingApiTest, MissingSegmentAndPathTraversalAre404) {
    upload("datos");
    queue.waitIdle();
    EXPECT_EQ(client().Get("/api/videos/" + kVideoId + "/dash/nada.m4s", auth("user-1"))->status,
              404);
    EXPECT_EQ(client().Get("/api/videos/" + kVideoId + "/dash/..%2F..%2Fsource", auth("user-1"))
                  ->status,
              404);
}

TEST_F(StreamingApiTest, InternalDeleteIsOnlyForServices) {
    upload("datos");
    queue.waitIdle();
    const std::string path = "/internal/videos/" + kVideoId;

    EXPECT_EQ(client().Delete(path)->status, 401);
    EXPECT_EQ(client().Delete(path, auth("user-1"))->status, 403);
    EXPECT_TRUE(fs::exists(store.dashDir(kVideoId)));

    EXPECT_EQ(client().Delete(path, auth("metadata-api", "service"))->status, 204);
    EXPECT_FALSE(fs::exists(store.dashDir(kVideoId)));
}

// --- Cliente HTTP de metadata, contra un servidor de metadata falso ---

TEST(HttpMetadataClientTest, ForwardsUserTokenAndUsesServiceTokenForStatus) {
    httplib::Server fake;
    std::string seenUserToken;
    std::optional<Claims> seenServiceClaims;
    nlohmann::json seenPatch;
    fake.Get("/api/videos/" + kVideoId, [&](const httplib::Request& req, httplib::Response& res) {
        seenUserToken = bearerToken(req);
        sendJson(res, 200, {{"authorId", "user-1"}, {"status", "pending"}, {"sizeBytes", 77}});
    });
    fake.Get("/api/videos/aaaaaaaaaaaaaaaaaaaaaaaa",
             [](const httplib::Request&, httplib::Response& res) {
                 res.set_content("esto no es json", "text/plain");
             });
    fake.Patch("/internal/videos/" + kVideoId,
               [&](const httplib::Request& req, httplib::Response& res) {
                   seenServiceClaims = verifyJwt(bearerToken(req), kSecret);
                   seenPatch = nlohmann::json::parse(req.body);
                   sendJson(res, 200, nlohmann::json::object());
               });
    const int port = fake.bind_to_any_port("127.0.0.1");
    std::thread listener([&] { fake.listen_after_bind(); });
    fake.wait_until_ready();

    HttpMetadataClient client("http://127.0.0.1:" + std::to_string(port), kSecret);

    const auto video = client.getVideo(kVideoId, "token-del-usuario");
    ASSERT_TRUE(video);
    EXPECT_EQ(video->authorId, "user-1");
    EXPECT_EQ(video->status, "pending");
    EXPECT_EQ(video->sizeBytes, 77u);
    EXPECT_EQ(seenUserToken, "token-del-usuario");

    EXPECT_FALSE(client.getVideo("ffffffffffffffffffffffff", "t")) << "404 de metadata";
    EXPECT_FALSE(client.getVideo("aaaaaaaaaaaaaaaaaaaaaaaa", "t")) << "respuesta no JSON";

    EXPECT_TRUE(client.reportStatus(kVideoId, "ready", 9.5, 1234));
    ASSERT_TRUE(seenServiceClaims);
    EXPECT_TRUE(seenServiceClaims->isService());
    EXPECT_EQ(seenPatch.at("status"), "ready");
    EXPECT_DOUBLE_EQ(seenPatch.at("durationSeconds").get<double>(), 9.5);
    EXPECT_EQ(seenPatch.at("sizeBytes"), 1234);

    EXPECT_TRUE(client.reportStatus(kVideoId, "failed", 0, 0));
    EXPECT_FALSE(seenPatch.contains("durationSeconds"));

    fake.stop();
    listener.join();

    EXPECT_FALSE(client.getVideo(kVideoId, "t")) << "metadata caído";
    EXPECT_FALSE(client.reportStatus(kVideoId, "ready", 1, 1));
}

// --- FFMPEG real ---

TEST(FfmpegTranscoderTest, CommandProducesTwoQualitiesIntoManifest) {
    const std::string command = FfmpegTranscoder::buildCommand("/data/a/source", "/data/a/dash");
    EXPECT_NE(command.find("-i '/data/a/source'"), std::string::npos);
    EXPECT_NE(command.find("scale=-2:360"), std::string::npos);
    EXPECT_NE(command.find("scale=-2:720"), std::string::npos);
    EXPECT_TRUE(command.ends_with("-f dash '/data/a/dash/manifest.mpd'"));
}

TEST(FfmpegTranscoderTest, TranscodesRealVideoToDash) {
    if (std::system("ffmpeg -version >/dev/null 2>&1") != 0) {
        GTEST_SKIP() << "ffmpeg no está instalado";
    }
    const fs::path dir = freshTempDir("ffmpeg");
    const fs::path source = dir / "source";
    const std::string generate =
        "ffmpeg -nostdin -loglevel error -y -f lavfi -i testsrc=duration=2:size=320x240:rate=24 "
        "-f lavfi -i sine=duration=2 -pix_fmt yuv420p -f mp4 '" + source.string() + "'";
    ASSERT_EQ(std::system(generate.c_str()), 0);

    FfmpegTranscoder transcoder;
    const auto duration = transcoder.transcode(source, dir / "dash");
    ASSERT_TRUE(duration);
    EXPECT_NEAR(*duration, 2.0, 0.2);

    std::ifstream manifestFile(dir / "dash" / "manifest.mpd");
    const std::string manifest((std::istreambuf_iterator<char>(manifestFile)), {});
    EXPECT_NE(manifest.find("height=\"360\""), std::string::npos);
    EXPECT_NE(manifest.find("height=\"720\""), std::string::npos);
    EXPECT_NE(manifest.find("audio/mp4"), std::string::npos);
    EXPECT_TRUE(fs::exists(dir / "dash" / "init-0.m4s"));
    EXPECT_TRUE(fs::exists(dir / "dash" / "chunk-0-00001.m4s"));

    const fs::path garbage = dir / "basura";
    std::ofstream(garbage) << "esto no es un video";
    EXPECT_FALSE(transcoder.transcode(garbage, dir / "dash-basura"));
    EXPECT_FALSE(transcoder.transcode(dir / "no-existe", dir / "dash-nada"));

    fs::remove_all(dir);
}
