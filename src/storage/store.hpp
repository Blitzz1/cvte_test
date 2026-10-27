#pragma once
#include "app/types.hpp"
struct sqlite3;
namespace avqm {
class Store {
  public:
    explicit Store(const std::filesystem::path &path);
    ~Store();
    void sample(const Json &snapshot);
    std::string event(const std::string &chain, const std::string &type, const Json &payload);
    Json history(const std::string &chain, int64_t from, int64_t to, int limit = 2000);
    Json events(const std::string &chain, int64_t from, int64_t to, int limit = 200);
    void prune(int days);

  private:
    sqlite3 *db_{};
    std::mutex mutex_;
    uint64_t sequence_{};
};
} // namespace avqm
