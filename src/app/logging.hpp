#pragma once
#include "app/types.hpp"
namespace avqm {
class Logger {
  public:
    static Logger &instance();
    void initialize(const std::filesystem::path &root, const std::string &level);
    void write(int level, std::string message);
    void set_level(const std::string &level);
    void set_retention(int days) {
        retention_days_ = days;
    }
    void stop();
    ~Logger();

  private:
    std::filesystem::path directory_;
    std::ofstream file_;
    std::mutex mutex_;
    std::deque<std::string> queue_;
    std::thread worker_;
    std::atomic<bool> done_{false};
    std::atomic<int> level_{32};
    std::atomic<int> retention_days_{7};
    uint64_t dropped_{};
    void loop();
};
} // namespace avqm
