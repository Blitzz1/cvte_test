#pragma once
#include "app/types.hpp"
namespace avqm {
class Services {
  public:
    explicit Services(Application &app);
    ~Services();
    std::string command(const std::string &chain, const std::string &action);
    Json command_status(const std::string &id);
    void notify(const Json &payload, const std::string &url);

  private:
    Application &app_;
    std::mutex mutex_;
    std::deque<Json> commands_, notifications_;
    std::map<std::string, Json> statuses_;
    std::thread control_, notifier_;
    std::atomic<bool> done_{false};
    uint64_t sequence_{};
    void control_loop();
    void notification_loop();
};
} // namespace avqm
