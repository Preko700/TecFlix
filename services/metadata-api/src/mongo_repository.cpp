#include <stdexcept>

#include <mongoc/mongoc.h>

#include "metadata.hpp"

// Los documentos se convierten entre nlohmann::json y BSON pasando por texto JSON
// ("extended JSON" de Mongo): es menos código que construir BSON campo por campo.
namespace tecflix::metadata {

namespace {

using nlohmann::json;

// Dueño de un bson_t: lo libera al salir de alcance (RAII).
class Bson {
public:
    explicit Bson(bson_t* document) : document_(document) {}
    ~Bson() { bson_destroy(document_); }
    Bson(const Bson&) = delete;
    Bson& operator=(const Bson&) = delete;
    bson_t* get() const { return document_; }

private:
    bson_t* document_;
};

[[noreturn]] void fail(const std::string& what, const bson_error_t& error) {
    throw std::runtime_error("MongoDB: " + what + ": " + error.message);
}

Bson parseBson(const std::string& text) {
    bson_error_t error;
    bson_t* document = bson_new_from_json(reinterpret_cast<const std::uint8_t*>(text.data()),
                                          static_cast<ssize_t>(text.size()), &error);
    if (document == nullptr) {
        fail("documento inválido", error);
    }
    return Bson(document);
}

Bson toBson(const json& value) {
    return parseBson(value.dump());
}

json toJson(const bson_t* document) {
    std::size_t length = 0;
    char* text = bson_as_relaxed_extended_json(document, &length);
    json value = json::parse(text, text + length);
    bson_free(text);
    return value;
}

json byId(const std::string& id) {
    return {{"_id", {{"$oid", id}}}};
}

Video toVideo(const json& document) {
    Video video = document.get<Video>();
    video.id = document.at("_id").at("$oid").get<std::string>();
    return video;
}

// Toma un cliente del pool mientras dura una operación y lo devuelve al terminar (RAII).
class Session {
public:
    Session(mongoc_client_pool_t* pool, const std::string& database)
        : pool_(pool),
          client_(mongoc_client_pool_pop(pool)),
          collection_(mongoc_client_get_collection(client_, database.c_str(), "videos")) {}
    ~Session() {
        mongoc_collection_destroy(collection_);
        mongoc_client_pool_push(pool_, client_);
    }
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;
    mongoc_collection_t* videos() const { return collection_; }

private:
    mongoc_client_pool_t* pool_;
    mongoc_client_t* client_;
    mongoc_collection_t* collection_;
};

// Recorre el cursor completo, lo destruye y devuelve los documentos.
std::vector<json> drain(mongoc_cursor_t* cursor) {
    std::vector<json> documents;
    const bson_t* document = nullptr;
    while (mongoc_cursor_next(cursor, &document)) {
        documents.push_back(toJson(document));
    }
    bson_error_t error;
    const bool failed = mongoc_cursor_error(cursor, &error);
    mongoc_cursor_destroy(cursor);
    if (failed) {
        fail("consulta", error);
    }
    return documents;
}

std::vector<json> findDocuments(const Session& session, const json& filter,
                                const json& options = json::object()) {
    return drain(mongoc_collection_find_with_opts(session.videos(), toBson(filter).get(),
                                                  toBson(options).get(), nullptr));
}

}  // namespace

MongoVideoRepository::MongoVideoRepository(const std::string& uri, std::string database)
    : pool_(nullptr), database_(std::move(database)) {
    mongoc_init();  // idempotente
    bson_error_t error;
    mongoc_uri_t* parsed = mongoc_uri_new_with_error(uri.c_str(), &error);
    if (parsed == nullptr) {
        fail("URI inválido", error);
    }
    pool_ = mongoc_client_pool_new(parsed);
    mongoc_uri_destroy(parsed);
}

MongoVideoRepository::~MongoVideoRepository() {
    mongoc_client_pool_destroy(pool_);
}

Video MongoVideoRepository::insert(Video video) {
    json fields = video;
    fields.erase("id");  // el id vive en _id
    const Bson document = toBson(fields);
    bson_oid_t oid;
    bson_oid_init(&oid, nullptr);
    BSON_APPEND_OID(document.get(), "_id", &oid);

    const Session session(pool_, database_);
    bson_error_t error;
    if (!mongoc_collection_insert_one(session.videos(), document.get(), nullptr, nullptr, &error)) {
        fail("insertar", error);
    }
    char text[25];
    bson_oid_to_string(&oid, text);
    video.id = text;
    return video;
}

std::optional<Video> MongoVideoRepository::find(const std::string& id) {
    const Session session(pool_, database_);
    const auto documents = findDocuments(session, byId(id));
    return documents.empty() ? std::nullopt : std::optional<Video>(toVideo(documents.front()));
}

std::optional<Video> MongoVideoRepository::update(const std::string& id, const json& changes) {
    {
        const Session session(pool_, database_);
        bson_error_t error;
        if (!mongoc_collection_update_one(session.videos(), toBson(byId(id)).get(),
                                          toBson({{"$set", changes}}).get(), nullptr, nullptr,
                                          &error)) {
            fail("actualizar", error);
        }
    }
    return find(id);
}

bool MongoVideoRepository::remove(const std::string& id) {
    const Session session(pool_, database_);
    bson_t reply;
    bson_error_t error;
    const bool ok = mongoc_collection_delete_one(session.videos(), toBson(byId(id)).get(), nullptr,
                                                 &reply, &error);
    bson_iter_t iterator;
    const bool deleted = ok && bson_iter_init_find(&iterator, &reply, "deletedCount") &&
                         bson_iter_as_int64(&iterator) > 0;
    bson_destroy(&reply);
    if (!ok) {
        fail("borrar", error);
    }
    return deleted;
}

VideoPage MongoVideoRepository::list(const ListQuery& query) {
    const json filter = {
        {"$or", json::array({{{"authorId", query.viewerId}},
                             {{"visibility", "public"}, {"status", "ready"}}})}};
    // ordered_json: en "sort" importa el orden de las claves y nlohmann::json las
    // reordena alfabéticamente (pondría _id antes del campo pedido).
    nlohmann::ordered_json sort;
    sort[query.sortField] = query.ascending ? 1 : -1;
    sort["_id"] = 1;  // desempata: sin él, valores iguales podrían repetirse entre páginas
    nlohmann::ordered_json options;
    options["sort"] = sort;
    options["skip"] = static_cast<std::int64_t>(query.page - 1) * query.pageSize;
    options["limit"] = query.pageSize;
    options["collation"] = {{"locale", "en"}, {"strength", 2}};  // sin distinguir mayúsculas

    const Session session(pool_, database_);
    bson_error_t error;
    const std::int64_t total = mongoc_collection_count_documents(
        session.videos(), toBson(filter).get(), nullptr, nullptr, nullptr, &error);
    if (total < 0) {
        fail("contar", error);
    }
    VideoPage page;
    page.total = static_cast<std::uint64_t>(total);
    for (const json& document :
         drain(mongoc_collection_find_with_opts(session.videos(), toBson(filter).get(),
                                                parseBson(options.dump()).get(), nullptr))) {
        page.items.push_back(toVideo(document));
    }
    return page;
}

std::uint64_t MongoVideoRepository::usedBytes(const std::string& authorId) {
    const json pipeline = {
        {"pipeline",
         json::array({{{"$match", {{"authorId", authorId}, {"status", {{"$ne", "failed"}}}}}},
                      {{"$group", {{"_id", nullptr}, {"total", {{"$sum", "$sizeBytes"}}}}}}})}};
    const Session session(pool_, database_);
    const auto result = drain(mongoc_collection_aggregate(
        session.videos(), MONGOC_QUERY_NONE, toBson(pipeline).get(), nullptr, nullptr));
    return result.empty() ? 0 : result.front().at("total").get<std::uint64_t>();
}

std::vector<std::string> MongoVideoRepository::removeByAuthor(const std::string& authorId) {
    const json filter = {{"authorId", authorId}};
    const Session session(pool_, database_);
    std::vector<std::string> ids;
    for (const json& document : findDocuments(session, filter, {{"projection", {{"_id", 1}}}})) {
        ids.push_back(document.at("_id").at("$oid").get<std::string>());
    }
    bson_error_t error;
    if (!mongoc_collection_delete_many(session.videos(), toBson(filter).get(), nullptr, nullptr,
                                       &error)) {
        fail("borrar videos del usuario", error);
    }
    return ids;
}

}  // namespace tecflix::metadata
