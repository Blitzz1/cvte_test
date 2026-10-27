#pragma once
#include "app/types.hpp"
#include <random>
namespace avqm {
struct SourceUnit {
    uint64_t id{};
    bool video{};
    std::string signature;
    int64_t source_us{}, media_us{}, duration_us{};
    bool matched{};
    FrameRecord frame;
};
class Chain {
  public:
    Application &app;
    ChainConfig cfg;
    mutable std::mutex mutex;
    std::atomic<bool> stop_requested{true}, publisher_ready{false};
    std::string state = "stopped", error, session;
    int width = 0, height = 0, sample_rate = 0;
    double fps = 0, bitrate = 0;
    std::deque<VideoImage> videos;
    std::deque<AudioChunk> audio;
    std::deque<std::shared_ptr<SourceUnit>> source;
    std::unordered_map<std::string, std::vector<std::weak_ptr<SourceUnit>>> signatures;
    std::vector<SyncSample> sync_samples;
    FreezeTracker freeze;
    FaultConfig fault;
    uint64_t sent = 0, matched_video = 0, decoded = 0, presented = 0, ambiguous = 0, received_bytes = 0;
    uint64_t source_sequence = 0, session_max_consecutive = 0;
    uint64_t playback_generation = 0;
    int64_t first_present_source_us = 0;
    uint64_t rtp_packets_received = 0, rtp_packets_lost = 0, rtp_packets_duplicates = 0;
    int64_t started_us = 0, playback_anchor_us = 0, first_rx_media = 0, last_video_media = 0,
            last_present_us = 0;
    std::optional<double> latency_ms, av_offset_ms;
    std::optional<int64_t> audio_clock_media;
    int64_t audio_clock_observed_us = 0;
    std::string output_validity = "warming_up", audio_validity = "warming_up";
    std::deque<std::pair<int64_t, double>> latency_samples;
    std::deque<int64_t> presentation_times;
    uint64_t received_video_units = 0;
    uint64_t receiver_connection = 0;
    std::deque<std::pair<int64_t, uint64_t>> byte_samples;
    int64_t last_audio_logged_us = 0;
    std::vector<AlertState> alarms{5};
    std::vector<Json> completed_freezes;
    explicit Chain(Application &a, ChainConfig c);
    ~Chain();
    void start();
    void stop();
    void set_fault(const Json &j);
    Json snapshot();
    void tick();
    void did_present(uint64_t id, int64_t media, int64_t source_time, int64_t observed);
    void did_drop(uint64_t id, const std::string &reason);
    void audio_progress(int64_t media, int64_t observed, uint64_t device_position = 0,
                        uint64_t device_frequency = 0);
    void rtp_frame_observed(const std::vector<uint8_t> &access_unit, size_t before, size_t after,
                            uint32_t timestamp);
    void injection_truth(const Json &value);

  private:
    std::thread publisher_, receiver_;
    void publish();
    void receive();
    std::shared_ptr<SourceUnit> match(const std::string &signature, bool video, int64_t pts);
    std::optional<int64_t> video_pts_offset_, audio_pts_offset_;
    std::ofstream observations_, truth_;
    void observe(const Json &j);
};
} // namespace avqm
