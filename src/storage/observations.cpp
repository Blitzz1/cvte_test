#include "storage/observations.hpp"
#include <algorithm>
#include <chrono>
#include <fstream>
#include <memory>
namespace avqm {
ObservationJournal::ObservationJournal(std::filesystem::path path, size_t capacity, bool autostart)
    : directory_(std::move(path)), capacity_(capacity) {
    std::filesystem::create_directories(directory_);
    if (autostart)
        start();
}
ObservationJournal::~ObservationJournal() {
    stop();
}
void ObservationJournal::start() {
    if (!worker_.joinable())
        worker_ = std::thread([this] { loop(); });
}
bool ObservationJournal::append(std::string session, std::string line, bool truth) {
    auto charge = sizeof(Entry) + session.size() + line.size();
    std::lock_guard lock(mutex_);
    if (done_ || session.empty() ||
        session.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_") !=
            std::string::npos ||
        charge > capacity_ || queued_ > capacity_ - charge) {
        ++dropped_[session];
        return false;
    }
    queued_ += charge;
    peak_ = std::max(peak_, queued_);
    queue_.push_back({std::move(session), std::move(line), truth, charge});
    ready_.notify_one();
    return true;
}
JournalStats ObservationJournal::stats(const std::string &session) {
    std::lock_guard lock(mutex_);
    return {queued_, peak_, dropped_[session], written_[session]};
}
void ObservationJournal::flush() {
    start();
    std::unique_lock lock(mutex_);
    drained_.wait(lock, [this] { return queue_.empty() && !writing_; });
}
void ObservationJournal::stop() {
    {
        std::lock_guard lock(mutex_);
        if (done_ && !worker_.joinable())
            return;
    }
    start();
    {
        std::lock_guard lock(mutex_);
        done_ = true;
    }
    ready_.notify_one();
    if (worker_.joinable())
        worker_.join();
}
void ObservationJournal::loop() {
    struct File {
        std::ofstream stream;
        std::filesystem::path path;
        std::string session;
        bool reported = false;
    };
    std::unordered_map<std::string, std::unique_ptr<File>> files;
    uint64_t part = 0;
    while (true) {
        std::deque<Entry> batch;
        {
            std::unique_lock lock(mutex_);
            ready_.wait_for(lock, std::chrono::milliseconds(100),
                            [this] { return done_ || !queue_.empty(); });
            if (queue_.empty() && done_)
                break;
            while (!queue_.empty() && batch.size() < 256) {
                batch.push_back(std::move(queue_.front()));
                queue_.pop_front();
            }
            writing_ = !batch.empty();
        }
        for (auto &item : batch) {
            bool success = false;
            try {
                auto key = item.session + (item.truth ? "-truth" : "");
                if (!files.contains(key)) {
                    if (files.size() >= 32)
                        files.erase(files.begin());
                    auto f = std::make_unique<File>();
                    f->session = item.session;
                    f->path = directory_ / (key + ".jsonl");
                    f->stream.open(f->path, std::ios::app);
                    files.emplace(key, std::move(f));
                }
                auto &f = *files.at(key);
                if (f.stream.tellp() > 100 * 1024 * 1024) {
                    f.stream.close();
                    auto stamp = std::chrono::duration_cast<std::chrono::milliseconds>(
                                     std::chrono::system_clock::now().time_since_epoch())
                                     .count();
                    f.path = directory_ / (key + "-part-" + std::to_string(stamp) + "-" +
                                           std::to_string(++part) + ".jsonl");
                    f.stream.open(f.path, std::ios::app);
                }
                f.stream << item.line << '\n';
                success = bool(f.stream);
            } catch (...) {
                success = false;
            }
            {
                std::lock_guard lock(mutex_);
                queued_ -= item.charge;
                if (success)
                    ++written_[item.session];
                else
                    ++dropped_[item.session];
            }
        }
        // No media lock is held by this worker, including during disk flush.
        for (auto &[key, file] : files) {
            file->stream.flush();
            if (!file->stream && !file->reported) {
                std::lock_guard lock(mutex_);
                ++dropped_[file->session];
                file->reported = true;
            }
        }
        {
            std::lock_guard lock(mutex_);
            writing_ = false;
            if (queue_.empty())
                drained_.notify_all();
        }
    }
    files.clear();
    {
        std::lock_guard lock(mutex_);
        writing_ = false;
        drained_.notify_all();
    }
}
} // namespace avqm
