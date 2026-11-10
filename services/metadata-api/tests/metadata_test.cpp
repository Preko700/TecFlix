#include <algorithm>
#include <cstdlib>
#include <thread>

#include <gtest/gtest.h>
#include <unistd.h>

#include "metadata.hpp"
#include "tecflix/http.hpp"

using namespace tecflix;
using namespace tecflix::metadata;
using nlohmann::json;

namespace {

const std::string kSecret = "secreto-de-prueba";
constexpr std::uint64_t kQuota = 1000;

httplib::Headers auth(const std::string& userId, const std::string& role = "user",
                      std::uint64_t quota = kQuota) {
    Claims claims;
    claims.sub = userId;
    claims.username = "nombre-" + userId;
    claims.role = role;
    claims.quota = quota;
    claims.exp = std::time(nullptr) + 60;
    return {{"Authorization", "Bearer " + signJwt(claims, kSecret)}};
}

// Repositorio en memoria con la misma semántica que el de Mongo.
class InMemoryRepository : public VideoRepository {
public:
    Video insert(Video video) override {
        char id[25];
        std::snprintf(id, sizeof(id), "%024x", static_cast<unsigned>(videos.size() + 1));
        video.id = id;
        videos.push_back(video);
        return video;
    }
    std::optional<Video> find(const std::string& id) override {
        const auto found = locate(id);
        return found == videos.end() ? std::nullopt : std::optional<Video>(*found);
    }
    std::optional<Video> update(const std::string& id, const json& changes) override {
        const auto found = locate(id);
        if (found == videos.end()) {
            return std::nullopt;
        }
        json merged = *found;
        merged.update(changes);
        *found = merged.get<Video>();
        return *found;
    }
    bool remove(const std::string& id) override {
        return std::erase_if(videos, [&](const Video& video) { return video.id == id; }) > 0;
    }
    VideoPage list(const ListQuery& query) override {
        lastQuery = query;
        std::vector<Video> visible;
        for (const Video& video : videos) {
            if (video.authorId == query.viewerId ||
                (video.visibility == "public" && video.status == "ready")) {
                visible.push_back(video);
            }
        }
        std::sort(visible.begin(), visible.end(), [&](const Video& a, const Video& b) {
            const json left = json(a)[query.sortField];
            const json right = json(b)[query.sortField];
            return query.ascending ? left < right : right < left;
        });
        VideoPage page;
        page.total = visible.size();
        const std::size_t first = static_cast<std::size_t>(query.page - 1) * query.pageSize;
        for (std::size_t i = first; i < visible.size() && i < first + query.pageSize; ++i) {
            page.items.push_back(visible[i]);
        }
        return page;
    }
    std::uint64_t usedBytes(const std::string& authorId) override {
        if (failing) {
            throw std::runtime_error("mongo caído");
        }
        std::uint64_t total = 0;
        for (const Video& video : videos) {
            if (video.authorId == authorId && video.status != "failed") {
                total += video.sizeBytes;
            }
        }
        return total;
    }
    std::vector<std::string> removeByAuthor(const std::string& authorId) override {
        std::vector<std::string> ids;
        for (const Video& video : videos) {
            if (video.authorId == authorId) {
                ids.push_back(video.id);
            }
        }
        std::erase_if(videos, [&](const Video& video) { return video.authorId == authorId; });
        return ids;
    }

    std::vector<Video> videos;
    ListQuery lastQuery;
    bool failing = false;

private:
    std::vector<Video>::iterator locate(const std::string& id) {
        return std::find_if(videos.begin(), videos.end(),
                            [&](const Video& video) { return video.id == id; });
    }
};

class FakeStreaming : public StreamingClient {
public:
    bool deleteFiles(const std::string& videoId) override {
        deleted.push_back(videoId);
        return true;
    }
    std::vector<std::string> deleted;
};

class MetadataApiTest : public ::testing::Test {
protected:
    void SetUp() override {
        api.registerRoutes(server);
        port = server.bind_to_any_port("127.0.0.1");
        listener = std::thread([this] { server.listen_after_bind(); });
        server.wait_until_ready();
    }
    void TearDown() override {
        server.stop();
        listener.join();
    }
    httplib::Client client() const { return httplib::Client("127.0.0.1", port); }

    httplib::Result create(const std::string& userId, const json& body) {
        return client().Post("/api/videos", auth(userId), body.dump(), "application/json");
    }
    // Crea un video válido y devuelve su id.
    std::string createVideo(const std::string& userId, const std::string& title,
                            const std::string& visibility = "public",
                            std::uint64_t sizeBytes = 100) {
        const auto res =
            create(userId, {{"title", title}, {"visibility", visibility}, {"sizeBytes", sizeBytes}});
        EXPECT_EQ(res->status, 201);
        return json::parse(res->body).at("id");
    }
    void markReady(const std::string& id) { repository.update(id, {{"status", "ready"}}); }
    httplib::Result patch(const std::string& path, const std::string& userId, const json& body,
                          const std::string& role = "user") {
        return client().Patch(path, auth(userId, role), body.dump(), "application/json");
    }
    std::vector<std::string> titles(const httplib::Result& res) {
        std::vector<std::string> result;
        const json body = json::parse(res->body);  // debe vivir durante todo el ciclo
        for (const json& item : body.at("items")) {
            result.push_back(item.at("title"));
        }
        return result;
    }

    InMemoryRepository repository;
    FakeStreaming streaming;
    MetadataApi api{{kSecret, "test-1"}, repository, streaming};
    httplib::Server server;
    int port = 0;
    std::thread listener;
};

}  // namespace

TEST_F(MetadataApiTest, HealthReportsService) {
    const auto res = client().Get("/health");
    ASSERT_TRUE(res);
    EXPECT_EQ(json::parse(res->body).at("service"), "metadata-api");
}

TEST_F(MetadataApiTest, EverythingRequiresToken) {
    EXPECT_EQ(client().Get("/api/videos")->status, 401);
    EXPECT_EQ(client().Post("/api/videos", "{}", "application/json")->status, 401);
}

TEST_F(MetadataApiTest, CreateStoresAuthorFromTokenAndStartsPending) {
    const auto res = create("u1", {{"title", "Gatos"},
                                   {"description", "video de gatos"},
                                   {"topic", "animales"},
                                   {"tags", {"gato", "humor"}},
                                   {"visibility", "private"},
                                   {"sizeBytes", 250},
                                   {"authorId", "otro"},     // se ignora: el autor sale del token
                                   {"status", "ready"}});    // se ignora
    ASSERT_EQ(res->status, 201);
    const json video = json::parse(res->body);
    EXPECT_EQ(video.at("id").get<std::string>().size(), 24u);
    EXPECT_EQ(video.at("title"), "Gatos");
    EXPECT_EQ(video.at("tags"), json({"gato", "humor"}));
    EXPECT_EQ(video.at("visibility"), "private");
    EXPECT_EQ(video.at("status"), "pending");
    EXPECT_EQ(video.at("authorId"), "u1");
    EXPECT_EQ(video.at("authorName"), "nombre-u1");
    EXPECT_EQ(video.at("sizeBytes"), 250);
    EXPECT_GT(video.at("uploadedAt").get<std::int64_t>(), 0);
}

TEST_F(MetadataApiTest, CreateValidatesBody) {
    const json valid = {{"title", "t"}, {"visibility", "public"}, {"sizeBytes", 1}};
    const auto with = [&](const std::string& key, const json& value) {
        json body = valid;
        body[key] = value;
        return body;
    };
    EXPECT_EQ(client().Post("/api/videos", auth("u1"), "no es json", "application/json")->status,
              400);
    EXPECT_EQ(create("u1", json::array())->status, 400);
    EXPECT_EQ(create("u1", {{"visibility", "public"}, {"sizeBytes", 1}})->status, 400);
    EXPECT_EQ(create("u1", {{"title", "t"}, {"sizeBytes", 1}})->status, 400);
    EXPECT_EQ(create("u1", {{"title", "t"}, {"visibility", "public"}})->status, 400);
    EXPECT_EQ(create("u1", with("title", ""))->status, 400);
    EXPECT_EQ(create("u1", with("title", 5))->status, 400);
    EXPECT_EQ(create("u1", with("visibility", "secreto"))->status, 400);
    EXPECT_EQ(create("u1", with("tags", "gato"))->status, 400);
    EXPECT_EQ(create("u1", with("tags", {1, 2}))->status, 400);
    EXPECT_EQ(create("u1", with("sizeBytes", 0))->status, 400);
    EXPECT_EQ(create("u1", with("sizeBytes", -5))->status, 400);
    EXPECT_EQ(create("u1", with("sizeBytes", "grande"))->status, 400);
    EXPECT_TRUE(repository.videos.empty());
}

TEST_F(MetadataApiTest, QuotaBlocksUploadsOnceReached) {
    createVideo("u1", "a", "public", 600);
    createVideo("u1", "b", "public", 400);  // llega exactamente a la cuota (1000)
    const auto res = create("u1", {{"title", "c"}, {"visibility", "public"}, {"sizeBytes", 1}});
    EXPECT_EQ(res->status, 409);
    EXPECT_EQ(create("u1", {{"title", "c"}, {"visibility", "public"}, {"sizeBytes", 5000}})->status,
              409);
    createVideo("u2", "de otro usuario", "public", 1000);  // la cuota es por usuario
}

TEST_F(MetadataApiTest, FailedAndDeletedVideosFreeQuota) {
    const std::string first = createVideo("u1", "a", "public", 1000);
    repository.update(first, {{"status", "failed"}});
    const std::string second = createVideo("u1", "b", "public", 1000);
    EXPECT_EQ(client().Delete("/api/videos/" + second, auth("u1"))->status, 204);
    createVideo("u1", "c", "public", 1000);
}

TEST_F(MetadataApiTest, UsageIsOnlyForOwnerOrServices) {
    createVideo("u1", "a", "public", 300);
    const auto own = client().Get("/api/users/u1/usage", auth("u1"));
    EXPECT_EQ(json::parse(own->body).at("usedBytes"), 300);
    EXPECT_EQ(client().Get("/api/users/u1/usage", auth("u2"))->status, 403);
    EXPECT_EQ(client().Get("/api/users/u1/usage", auth("user-api", "service"))->status, 200);
}

TEST_F(MetadataApiTest, GetHidesPrivateVideosOfOthers) {
    const std::string privateId = createVideo("u1", "privado", "private");
    const std::string publicId = createVideo("u1", "publico", "public");

    EXPECT_EQ(client().Get("/api/videos/" + privateId, auth("u1"))->status, 200);
    EXPECT_EQ(client().Get("/api/videos/" + privateId, auth("u2"))->status, 404);
    EXPECT_EQ(client().Get("/api/videos/" + privateId, auth("streaming-api", "service"))->status,
              200);
    EXPECT_EQ(client().Get("/api/videos/" + publicId, auth("u2"))->status, 200);
    EXPECT_EQ(client().Get("/api/videos/ffffffffffffffffffffffff", auth("u1"))->status, 404);
}

TEST_F(MetadataApiTest, ListShowsOwnVideosAndReadyPublicOnes) {
    createVideo("u1", "mio privado", "private");
    createVideo("u1", "mio pendiente", "public");
    createVideo("u2", "ajeno privado", "private");
    createVideo("u2", "ajeno pendiente", "public");
    markReady(createVideo("u2", "ajeno listo", "public"));

    const auto res = client().Get("/api/videos?sort=title", auth("u1"));
    ASSERT_EQ(res->status, 200);
    EXPECT_EQ(titles(res),
              (std::vector<std::string>{"ajeno listo", "mio pendiente", "mio privado"}));
    EXPECT_EQ(json::parse(res->body).at("total"), 3);
}

TEST_F(MetadataApiTest, ListSortsByTitleAuthorAndDate) {
    markReady(createVideo("u2", "B"));
    markReady(createVideo("u1", "C"));
    markReady(createVideo("u3", "A"));
    repository.videos[0].uploadedAt = 30;
    repository.videos[1].uploadedAt = 10;
    repository.videos[2].uploadedAt = 20;
    using Titles = std::vector<std::string>;

    EXPECT_EQ(titles(client().Get("/api/videos?sort=title", auth("u1"))), (Titles{"A", "B", "C"}));
    EXPECT_EQ(titles(client().Get("/api/videos?sort=title&order=desc", auth("u1"))),
              (Titles{"C", "B", "A"}));
    EXPECT_EQ(titles(client().Get("/api/videos?sort=author", auth("u1"))),
              (Titles{"C", "B", "A"}));
    EXPECT_EQ(repository.lastQuery.sortField, "authorName");
    EXPECT_EQ(titles(client().Get("/api/videos?sort=uploadedAt&order=asc", auth("u1"))),
              (Titles{"C", "A", "B"}));
    // Sin parámetros: los más recientes primero.
    EXPECT_EQ(titles(client().Get("/api/videos", auth("u1"))), (Titles{"B", "A", "C"}));
}

TEST_F(MetadataApiTest, ListPaginates) {
    for (const char* title : {"a", "b", "c", "d", "e"}) {
        createVideo("u1", title);
    }
    const auto second = client().Get("/api/videos?sort=title&page=2&pageSize=2", auth("u1"));
    EXPECT_EQ(titles(second), (std::vector<std::string>{"c", "d"}));
    const json body = json::parse(second->body);
    EXPECT_EQ(body.at("total"), 5);
    EXPECT_EQ(body.at("page"), 2);
    EXPECT_EQ(body.at("pageSize"), 2);

    EXPECT_EQ(titles(client().Get("/api/videos?sort=title&page=3&pageSize=2", auth("u1"))),
              (std::vector<std::string>{"e"}));
    EXPECT_TRUE(titles(client().Get("/api/videos?page=9", auth("u1"))).empty());
}

TEST_F(MetadataApiTest, ListRejectsBadParameters) {
    for (const char* query : {"page=0", "page=abc", "page=2x", "pageSize=0", "pageSize=101",
                              "sort=duracion", "order=arriba"}) {
        EXPECT_EQ(client().Get(std::string("/api/videos?") + query, auth("u1"))->status, 400)
            << query;
    }
}

TEST_F(MetadataApiTest, AuthorCanEditMetadataAndVisibility) {
    const std::string id = createVideo("u1", "viejo", "private");
    const auto res = patch("/api/videos/" + id, "u1",
                           {{"title", "nuevo"}, {"visibility", "public"}, {"tags", {"x"}}});
    ASSERT_EQ(res->status, 200);
    const json video = json::parse(res->body);
    EXPECT_EQ(video.at("title"), "nuevo");
    EXPECT_EQ(video.at("visibility"), "public");
    EXPECT_EQ(video.at("tags"), json({"x"}));
    EXPECT_EQ(video.at("authorId"), "u1");
}

TEST_F(MetadataApiTest, PatchCannotTouchProtectedFieldsOrOthersVideos) {
    const std::string id = createVideo("u1", "titulo", "public");
    const std::string path = "/api/videos/" + id;

    EXPECT_EQ(patch(path, "u2", {{"title", "hackeado"}})->status, 403);
    EXPECT_EQ(patch(path, "u1", {{"status", "ready"}, {"sizeBytes", 1}})->status, 400)
        << "solo campos no editables";
    EXPECT_EQ(patch(path, "u1", {{"title", ""}})->status, 400);
    EXPECT_EQ(patch(path, "u1", json::array())->status, 400);
    EXPECT_EQ(patch("/api/videos/ffffffffffffffffffffffff", "u1", {{"title", "x"}})->status, 404);

    const std::string privateId = createVideo("u1", "privado", "private");
    EXPECT_EQ(patch("/api/videos/" + privateId, "u2", {{"title", "x"}})->status, 404);
    EXPECT_EQ(repository.videos[0].title, "titulo");
    EXPECT_EQ(repository.videos[0].status, "pending");
}

TEST_F(MetadataApiTest, DeleteRemovesVideoAndItsFiles) {
    const std::string id = createVideo("u1", "titulo");
    EXPECT_EQ(client().Delete("/api/videos/" + id, auth("u2"))->status, 403);
    EXPECT_TRUE(streaming.deleted.empty());

    EXPECT_EQ(client().Delete("/api/videos/" + id, auth("u1"))->status, 204);
    EXPECT_EQ(streaming.deleted, std::vector<std::string>{id});
    EXPECT_EQ(client().Get("/api/videos/" + id, auth("u1"))->status, 404);
    EXPECT_EQ(client().Delete("/api/videos/" + id, auth("u1"))->status, 404);
}

TEST_F(MetadataApiTest, InternalStatusUpdateIsOnlyForServices) {
    const std::string id = createVideo("u1", "titulo");
    const std::string path = "/internal/videos/" + id;

    EXPECT_EQ(patch(path, "u1", {{"status", "ready"}})->status, 403);

    const auto res = patch(path, "streaming-api",
                           {{"status", "ready"}, {"durationSeconds", 12.5}, {"sizeBytes", 90}},
                           "service");
    ASSERT_EQ(res->status, 200);
    const json video = json::parse(res->body);
    EXPECT_EQ(video.at("status"), "ready");
    EXPECT_DOUBLE_EQ(video.at("durationSeconds").get<double>(), 12.5);
    EXPECT_EQ(video.at("sizeBytes"), 90);

    EXPECT_EQ(patch(path, "s", {{"status", "volando"}}, "service")->status, 400);
    EXPECT_EQ(patch(path, "s", {{"durationSeconds", 1}}, "service")->status, 400);
    EXPECT_EQ(patch(path, "s", {{"status", "ready"}, {"durationSeconds", "x"}}, "service")->status,
              400);
    EXPECT_EQ(patch(path, "s", {{"status", "ready"}, {"sizeBytes", -1}}, "service")->status, 400);
    EXPECT_EQ(patch(path, "s", "texto", "service")->status, 400);
    EXPECT_EQ(patch("/internal/videos/ffffffffffffffffffffffff", "s", {{"status", "ready"}},
                    "service")
                  ->status,
              404);
}

TEST_F(MetadataApiTest, DeletingAUserRemovesAllTheirVideos) {
    const std::string first = createVideo("u1", "a");
    const std::string second = createVideo("u1", "b");
    createVideo("u2", "c");
    const std::string path = "/internal/users/u1/videos";

    EXPECT_EQ(client().Delete(path, auth("u1"))->status, 403);
    EXPECT_EQ(client().Delete(path, auth("user-api", "service"))->status, 204);
    EXPECT_EQ(streaming.deleted, (std::vector<std::string>{first, second}));
    ASSERT_EQ(repository.videos.size(), 1u);
    EXPECT_EQ(repository.videos[0].authorId, "u2");
}

TEST_F(MetadataApiTest, RepositoryFailureIsA500WithJsonError) {
    repository.failing = true;
    const auto res = client().Get("/api/users/u1/usage", auth("u1"));
    EXPECT_EQ(res->status, 500);
    EXPECT_EQ(json::parse(res->body).at("error"), "error interno");
}

// --- Cliente HTTP de streaming, contra un servidor falso ---

TEST(HttpStreamingClientTest, DeletesWithServiceToken) {
    httplib::Server fake;
    std::optional<Claims> seen;
    fake.Delete("/internal/videos/abc", [&](const httplib::Request& req, httplib::Response& res) {
        seen = verifyJwt(bearerToken(req), kSecret);
        res.status = 204;
    });
    const int port = fake.bind_to_any_port("127.0.0.1");
    std::thread listener([&] { fake.listen_after_bind(); });
    fake.wait_until_ready();

    HttpStreamingClient client("http://127.0.0.1:" + std::to_string(port), kSecret);
    EXPECT_TRUE(client.deleteFiles("abc"));
    ASSERT_TRUE(seen);
    EXPECT_TRUE(seen->isService());
    EXPECT_FALSE(client.deleteFiles("no-existe"));

    fake.stop();
    listener.join();
    EXPECT_FALSE(client.deleteFiles("abc")) << "streaming caído";
}

// --- MongoDB real: corre solo si MONGO_TEST_URI apunta a un servidor (en CI sí) ---

class MongoRepositoryTest : public ::testing::Test {
protected:
    void SetUp() override {
        const char* uri = std::getenv("MONGO_TEST_URI");
        if (uri == nullptr) {
            GTEST_SKIP() << "definir MONGO_TEST_URI para probar contra MongoDB";
        }
        repository = std::make_unique<MongoVideoRepository>(uri, "tecflix_test");
        repository->removeByAuthor(alice);
        repository->removeByAuthor(bob);
    }
    void TearDown() override {
        if (repository) {
            repository->removeByAuthor(alice);
            repository->removeByAuthor(bob);
        }
    }
    Video make(const std::string& author, const std::string& title, std::int64_t uploadedAt,
               const std::string& visibility = "public", const std::string& status = "ready",
               std::uint64_t sizeBytes = 100) {
        Video video;
        video.title = title;
        video.tags = {"t1", "t2"};
        video.visibility = visibility;
        video.status = status;
        video.authorId = author;
        video.authorName = author;
        video.sizeBytes = sizeBytes;
        video.uploadedAt = uploadedAt;
        return repository->insert(video);
    }
    std::vector<std::string> titles(const ListQuery& query) {
        std::vector<std::string> result;
        for (const Video& video : repository->list(query).items) {
            result.push_back(video.title);
        }
        return result;
    }

    // Autores únicos por proceso: las pruebas no chocan con otros datos de la base.
    const std::string alice = "alice-" + std::to_string(::getpid());
    const std::string bob = "bob-" + std::to_string(::getpid());
    std::unique_ptr<MongoVideoRepository> repository;
};

TEST_F(MongoRepositoryTest, InsertFindUpdateRemoveRoundTrip) {
    const Video created = make(alice, "Gatos", 5, "private", "pending", 5'000'000'000);
    EXPECT_EQ(created.id.size(), 24u);

    const auto found = repository->find(created.id);
    ASSERT_TRUE(found);
    EXPECT_EQ(found->title, "Gatos");
    EXPECT_EQ(found->tags, (std::vector<std::string>{"t1", "t2"}));
    EXPECT_EQ(found->sizeBytes, 5'000'000'000u) << "tamaños mayores a 4 GB";
    EXPECT_EQ(found->uploadedAt, 5);

    const auto updated =
        repository->update(created.id, {{"status", "ready"}, {"durationSeconds", 12.5}});
    ASSERT_TRUE(updated);
    EXPECT_EQ(updated->status, "ready");
    EXPECT_DOUBLE_EQ(updated->durationSeconds, 12.5);
    EXPECT_EQ(updated->title, "Gatos") << "los campos no enviados no cambian";

    EXPECT_TRUE(repository->remove(created.id));
    EXPECT_FALSE(repository->remove(created.id));
    EXPECT_FALSE(repository->find(created.id));
    EXPECT_FALSE(repository->update(created.id, {{"status", "ready"}}));
}

TEST_F(MongoRepositoryTest, ListFiltersSortsAndPaginates) {
    make(alice, "banana", 30);
    make(alice, "Cereza", 10, "private");
    make(bob, "ajeno privado", 1, "private");
    make(bob, "ajeno pendiente", 2, "public", "pending");
    make(bob, "Arandano", 20);

    ListQuery query;
    query.viewerId = alice;
    query.sortField = "title";
    query.ascending = true;
    // Orden alfabético sin distinguir mayúsculas; nada privado o pendiente de bob.
    // (Puede haber videos públicos de otros datos: se filtran los de esta prueba.)
    EXPECT_EQ(repository->list(query).total >= 3, true);
    query.pageSize = 100;
    std::vector<std::string> mine;
    for (const std::string& title : titles(query)) {
        if (title == "banana" || title == "Cereza" || title == "Arandano" ||
            title.starts_with("ajeno")) {
            mine.push_back(title);
        }
    }
    EXPECT_EQ(mine, (std::vector<std::string>{"Arandano", "banana", "Cereza"}));

    query.sortField = "uploadedAt";
    query.ascending = false;
    query.pageSize = 1;
    const auto newest = repository->list(query);
    EXPECT_EQ(newest.items.size(), 1u);
    query.page = 2;
    EXPECT_NE(repository->list(query).items.front().id, newest.items.front().id);
}

TEST_F(MongoRepositoryTest, UsedBytesIgnoresFailedVideos) {
    EXPECT_EQ(repository->usedBytes(alice), 0u);
    make(alice, "a", 1, "public", "ready", 3'000'000'000);
    make(alice, "b", 2, "public", "pending", 3'000'000'000);
    make(alice, "c", 3, "public", "failed", 999);
    make(bob, "d", 4, "public", "ready", 50);
    EXPECT_EQ(repository->usedBytes(alice), 6'000'000'000u);
}

TEST_F(MongoRepositoryTest, RemoveByAuthorReturnsDeletedIds) {
    const Video first = make(alice, "a", 1);
    const Video second = make(alice, "b", 2);
    const Video other = make(bob, "c", 3);

    auto ids = repository->removeByAuthor(alice);
    std::sort(ids.begin(), ids.end());
    std::vector<std::string> expected = {first.id, second.id};
    std::sort(expected.begin(), expected.end());
    EXPECT_EQ(ids, expected);
    EXPECT_FALSE(repository->find(first.id));
    EXPECT_TRUE(repository->find(other.id));
}

TEST(MongoRepositoryErrors, InvalidUriThrows) {
    EXPECT_THROW(MongoVideoRepository("esto no es un uri", "x"), std::runtime_error);
}
