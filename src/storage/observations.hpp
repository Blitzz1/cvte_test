#pragma once
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
namespace avqm {
struct JournalStats {
    size_t queued_bytes{}, peak_bytes{};
    uint64_t dropped{}, written{};
};
class ObservationJournal {
  public:
    explicit ObservationJournal(std::filesystem::path directory, size_t capacity = 8 * 1024 * 1024,
                                bool autostart = true);
    ~ObservationJournal();
    bool append(std::string session, std::string line, bool truth = false);
    JournalStats stats(const std::string &session);
    void start();
    void flush();
    void stop();

  private:
    struct Entry {
        std::string session, line;
        bool truth;
        size_t charge;
    };
    std::filesystem::path directory_;
    size_t capacity_, queued_{}, peak_{};
    std::mutex mutex_;
    std::condition_variable ready_, drained_;
    std::deque<Entry> queue_;
    std::unordered_map<std::string, uint64_t> dropped_, written_;
    std::thread worker_;
    bool done_ = false, writing_ = false;
    void loop();
};
} // namespace avqm
