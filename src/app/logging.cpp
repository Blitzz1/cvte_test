#include "app/logging.hpp"
#include <cstdarg>
extern "C" {
#include <libavutil/log.h>
}
namespace avqm {
static void ffmpeg_log(void *, int level, const char *format, va_list args) {
    char text[4096]{};
    vsnprintf(text, sizeof(text), format, args);
    try {
        Logger::instance().write(level, std::string("ffmpeg: ") + text);
    } catch (...) {
    }
}
Logger &Logger::instance() {
    static Logger value;
    return value;
}
Logger::~Logger() {
    stop();
}
void Logger::set_level(const std::string &value) {
    int l = value == "trace"     ? 56
            : value == "debug"   ? 48
            : value == "warning" ? 24
            : value == "error"   ? 16
                                 : 32;
    level_ = l;
    av_log_set_level(l);
}
void Logger::initialize(const std::filesystem::path &root, const std::string &level) {
    directory_ = std::filesystem::absolute(root / "data/logs");
    std::filesystem::create_directories(directory_);
    auto rel = std::filesystem::weakly_canonical(directory_)
                   .lexically_relative(std::filesystem::weakly_canonical(root));
    if (rel.empty() || *rel.begin() == "..")
        throw std::runtime_error("Log directory must remain inside project");
    file_.open(directory_ / "runtime.log", std::ios::app);
    if (!file_)
        throw std::runtime_error("Cannot write runtime log");
    set_level(level);
    done_ = false;
    worker_ = std::thread([this] { loop(); });
    av_log_set_callback(ffmpeg_log);
}
void Logger::write(int level, std::string message) {
    if (level > level_)
        return;
    std::lock_guard lock(mutex_);
    if (queue_.size() >= 4096) {
        ++dropped_;
        return;
    }
    queue_.push_back(std::to_string(utc_ms()) + " level=" + std::to_string(level) + " " + message);
}
void Logger::loop() {
    auto day = utc_ms() / 86400000;
    int64_t next_prune = 0;
    while (true) {
        std::deque<std::string> batch;
        {
            std::lock_guard lock(mutex_);
            batch.swap(queue_);
            if (dropped_) {
                batch.push_back("logging_queue_overflow dropped=" + std::to_string(dropped_));
                dropped_ = 0;
            }
        }
        for (auto &line : batch)
            file_ << line << (line.ends_with('\n') ? "" : "\n");
        file_.flush();
        if (file_ && (file_.tellp() > 10 * 1024 * 1024 || utc_ms() / 86400000 != day)) {
            file_.close();
            std::error_code ec;
            std::filesystem::rename(directory_ / "runtime.log",
                                    directory_ / ("runtime-" + std::to_string(utc_ms()) + ".log"), ec);
            file_.open(directory_ / "runtime.log", std::ios::app);
            day = utc_ms() / 86400000;
        }
        if (now_us() >= next_prune) {
            std::error_code ec;
            for (auto &entry : std::filesystem::directory_iterator(directory_, ec)) {
                if (entry.path().filename() != "runtime.log" && entry.is_regular_file(ec) &&
                    std::filesystem::file_time_type::clock::now() - entry.last_write_time(ec) >
                        std::chrono::hours(retention_days_ * 24))
                    std::filesystem::remove(entry.path(), ec);
            }
            next_prune = now_us() + 60000000;
        }
        if (done_) {
            std::lock_guard lock(mutex_);
            if (queue_.empty())
                break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}
void Logger::stop() {
    done_ = true;
    if (worker_.joinable())
        worker_.join();
    if (file_.is_open())
        file_.close();
    av_log_set_callback(av_log_default_callback);
}
} // namespace avqm
