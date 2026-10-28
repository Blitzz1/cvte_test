#include "storage/store.hpp"
#include <sqlite3.h>
#include <stdexcept>
using namespace avqm;
static void check(bool condition, const char *message) {
    if (!condition)
        throw std::runtime_error(message);
}
int main() {
    try {
        const auto dir = std::filesystem::current_path() / "storage-tests" / std::to_string(now_us());
        std::filesystem::create_directories(dir);
        const auto path = dir / "quality.db";
        std::string retained_event;
        {
            Store store(path);
            store.sample({{"id", "fixture"},
                          {"fixture", "expired"},
                          {"latency_ms", 1000},
                          {"av_offset_ms", 200},
                          {"loss_pct", 20}});
            store.sample({{"id", "fixture"},
                          {"fixture", "kept"},
                          {"latency_ms", 400},
                          {"av_offset_ms", -10},
                          {"loss_pct", 0}});
            store.event("fixture", "expired", {{"marker", "expired"}});
            retained_event = store.event("fixture", "kept", {{"marker", "survives_restart"}});
        }
        // Age only this isolated fixture, without changing the system clock or production data.
        sqlite3 *db{};
        check(sqlite3_open(path.string().c_str(), &db) == SQLITE_OK, "open fixture database");
        const auto old = std::to_string(utc_ms() - 10LL * 86400000);
        const auto sql =
            "UPDATE samples SET time_ms=" + old +
            " WHERE json_extract(payload,'$.fixture')='expired'; UPDATE events SET time_ms=" + old +
            " WHERE type='expired';";
        const auto rc = sqlite3_exec(db, sql.c_str(), nullptr, nullptr, nullptr);
        sqlite3_close(db);
        check(rc == SQLITE_OK, "age isolated test records");
        {
            Store reopened(path);
            const auto from = utc_ms() - 11LL * 86400000, to = utc_ms() + 1000;
            auto samples = reopened.history("fixture", from, to);
            auto events = reopened.events("fixture", from, to);
            check(samples.size() == 2 && events.size() == 2, "samples and events survive restart");
            reopened.prune(7);
            samples = reopened.history("fixture", from, to);
            events = reopened.events("fixture", from, to);
            check(samples.size() == 1 && samples[0]["fixture"] == "kept", "expired samples removed only");
            check(events.size() == 1 && events[0]["id"] == retained_event &&
                      events[0]["payload"]["marker"] == "survives_restart",
                  "expired events removed while retained identity and payload survive");
            check(reopened.history("other", from, to).empty(), "chain queries remain isolated");
            check(reopened.events("fixture", to + 1000, to + 2000).empty(),
                  "empty ranges are not fabricated");
        }
        std::cout << "Storage persistence and retention cases passed\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
