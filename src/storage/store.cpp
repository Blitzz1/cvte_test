#include "storage/store.hpp"
#include <sqlite3.h>
namespace avqm {
static void exec(sqlite3 *db, const char *sql) {
    char *e = nullptr;
    if (sqlite3_exec(db, sql, nullptr, nullptr, &e) != SQLITE_OK) {
        std::string s = e ? e : "sqlite error";
        sqlite3_free(e);
        throw std::runtime_error(s);
    }
}
Store::Store(const std::filesystem::path &p) {
    std::filesystem::create_directories(p.parent_path());
    if (sqlite3_open(p.string().c_str(), &db_) != SQLITE_OK)
        throw std::runtime_error("database open failed");
    sqlite3_busy_timeout(db_, 2000);
    exec(
        db_,
        "PRAGMA journal_mode=WAL; PRAGMA synchronous=NORMAL; CREATE TABLE IF NOT EXISTS samples(time_ms "
        "INTEGER,chain TEXT,payload TEXT); CREATE INDEX IF NOT EXISTS samples_time ON "
        "samples(chain,time_ms); CREATE TABLE IF NOT EXISTS events(id TEXT PRIMARY KEY,time_ms INTEGER,chain "
        "TEXT,type TEXT,payload TEXT); CREATE INDEX IF NOT EXISTS events_time ON events(chain,time_ms);");
}
Store::~Store() {
    if (db_)
        sqlite3_close(db_);
}
void Store::sample(const Json &j) {
    std::lock_guard lock(mutex_);
    sqlite3_stmt *st{};
    sqlite3_prepare_v2(db_, "INSERT INTO samples VALUES(?,?,?)", -1, &st, nullptr);
    auto s = j.dump();
    auto chain = j.at("id").get<std::string>();
    sqlite3_bind_int64(st, 1, utc_ms());
    sqlite3_bind_text(st, 2, chain.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 3, s.c_str(), -1, SQLITE_TRANSIENT);
    auto rc = sqlite3_step(st);
    sqlite3_finalize(st);
    if (rc != SQLITE_DONE)
        throw std::runtime_error("database sample write failed");
}
std::string Store::event(const std::string &c, const std::string &t, const Json &j) {
    std::lock_guard lock(mutex_);
    auto stamp = utc_ms();
    std::string id = std::to_string(stamp) + "-" + std::to_string(++sequence_);
    sqlite3_stmt *st{};
    sqlite3_prepare_v2(db_, "INSERT INTO events VALUES(?,?,?,?,?)", -1, &st, nullptr);
    auto s = j.dump();
    sqlite3_bind_text(st, 1, id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st, 2, stamp);
    sqlite3_bind_text(st, 3, c.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 4, t.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 5, s.c_str(), -1, SQLITE_TRANSIENT);
    auto rc = sqlite3_step(st);
    sqlite3_finalize(st);
    if (rc != SQLITE_DONE)
        throw std::runtime_error("database event write failed");
    return id;
}
Json Store::history(const std::string &c, int64_t from, int64_t to, int limit) {
    std::lock_guard lock(mutex_);
    sqlite3_stmt *st{};
    int64_t bucket = std::max<int64_t>(1000, (to - from) / std::clamp(limit, 10, 5000));
    sqlite3_prepare_v2(db_,
                       "SELECT time_ms,payload FROM samples WHERE chain=? AND time_ms>=? AND time_ms<? GROUP "
                       "BY (time_ms/? ) ORDER BY time_ms LIMIT 5000",
                       -1, &st, nullptr);
    sqlite3_bind_text(st, 1, c.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st, 2, from);
    sqlite3_bind_int64(st, 3, to);
    sqlite3_bind_int64(st, 4, bucket);
    Json out = Json::array();
    while (sqlite3_step(st) == SQLITE_ROW) {
        auto j = Json::parse(reinterpret_cast<const char *>(sqlite3_column_text(st, 1)));
        j["time_ms"] = sqlite3_column_int64(st, 0);
        out.push_back(std::move(j));
    }
    sqlite3_finalize(st);
    return out;
}
Json Store::events(const std::string &c, int64_t from, int64_t to, int limit) {
    std::lock_guard lock(mutex_);
    sqlite3_stmt *st{};
    sqlite3_prepare_v2(db_,
                       "SELECT id,time_ms,chain,type,payload FROM events WHERE (?='' OR chain=?) AND "
                       "time_ms>=? AND time_ms<? ORDER BY time_ms DESC LIMIT ?",
                       -1, &st, nullptr);
    sqlite3_bind_text(st, 1, c.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, c.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st, 3, from);
    sqlite3_bind_int64(st, 4, to);
    sqlite3_bind_int(st, 5, std::clamp(limit, 1, 2000));
    Json out = Json::array();
    while (sqlite3_step(st) == SQLITE_ROW)
        out.push_back({{"id", reinterpret_cast<const char *>(sqlite3_column_text(st, 0))},
                       {"time_ms", sqlite3_column_int64(st, 1)},
                       {"chain", reinterpret_cast<const char *>(sqlite3_column_text(st, 2))},
                       {"type", reinterpret_cast<const char *>(sqlite3_column_text(st, 3))},
                       {"payload", Json::parse(reinterpret_cast<const char *>(sqlite3_column_text(st, 4)))}});
    sqlite3_finalize(st);
    return out;
}
void Store::prune(int days) {
    std::lock_guard lock(mutex_);
    auto cutoff = utc_ms() - int64_t(std::max(1, days)) * 86400000;
    for (const char *table : {"samples", "events"}) {
        auto sql = "DELETE FROM " + std::string(table) + " WHERE time_ms<" + std::to_string(cutoff);
        exec(db_, sql.c_str());
    }
    exec(db_, "PRAGMA wal_checkpoint(PASSIVE)");
}
} // namespace avqm
