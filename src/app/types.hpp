#pragma once
#include "detection/core.hpp"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <thread>
#include <unordered_map>
namespace avqm {
using Json = nlohmann::json;
inline int64_t now_us() {
    return std::chrono::duration_cast<std::chrono::microseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}
inline int64_t utc_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}
inline void wait_until(int64_t due, const std::atomic<bool> &stop) {
    while (!stop) {
        auto d = due - now_us();
        if (d <= 0)
            break;
        std::this_thread::sleep_for(std::chrono::microseconds(std::min<int64_t>(d, 10000)));
    }
}
struct Settings {
    double delay_warning_ms = 400, delay_critical_ms = 1000, loss_warning_pct = 1, loss_critical_pct = 5;
    double av_lead_warning_ms = 45, av_lag_warning_ms = 125, av_lead_critical_ms = 90,
           av_lag_critical_ms = 185, drift_warning_ms_per_s = 1;
    int freeze_critical_ms = 1000, drift_hold_ms = 30000;
    int hold_ms = 3000, recovery_ms = 3000, retention_days = 7, resolution_timeout_ms = 5000;
    std::string webhook;
};
Settings parse_settings(const Json &value);
struct FaultConfig {
    double packet_loss_pct = 0, frame_loss_pct = 0, presentation_drop_pct = 0;
    int delay_ms = 0, audio_delay_ms = 0, video_delay_ms = 0;
    double drift_ms_per_s = 0;
    uint32_t seed = 1;
    int burst_every = 0, burst_length = 0;
    int64_t started_us = 0;
};
struct ChainConfig {
    std::string id, alias, source, protocol = "rtsp", transport = "tcp", publish_url, pull_url;
    bool enabled = true;
    bool push_enabled = true, has_audio = true;
};
struct VideoImage {
    int width{}, height{};
    std::vector<uint8_t> bgra;
    uint64_t frame_id{};
    int64_t source_us{}, media_us{}, due_us{};
};
struct AudioChunk {
    std::vector<float> samples;
    int64_t media_us{}, due_us{};
    bool mapped{};
};
class Store;
class Chain;
class Output;
class RtspRelay;
class Services;
class Application {
  public:
    std::filesystem::path root;
    std::filesystem::path config_file;
    std::vector<std::shared_ptr<Chain>> chains;
    std::unique_ptr<Store> store;
    std::unique_ptr<Output> output;
    std::unique_ptr<RtspRelay> relay;
    std::unique_ptr<Services> services;
    std::atomic<bool> quit{false};
    std::mutex config_mutex;
    Json config;
    Settings settings;
    std::string token;
    explicit Application(std::filesystem::path r, std::filesystem::path config_path = "config/app.json");
    ~Application();
    Settings settings_copy();
    Json snapshot();
    std::shared_ptr<Chain> find(const std::string &id);
    void event(std::string chain, std::string type, Json payload);
};
} // namespace avqm
