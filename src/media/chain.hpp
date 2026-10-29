#pragma once
#include "app/types.hpp"
#include "playback/preview.hpp"
#include <random>
namespace avqm {
struct SourceUnit {
    uint64_t id{};
    bool video{};
    std::string signature;
    int64_t source_us{}, media_us{}, duration_us{};
    bool matched{};
    std::optional<int64_t> received_media_us;
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
    std::unordered_map<uint64_t, std::weak_ptr<SourceUnit>> source_ids;
    std::multimap<int64_t, std::weak_ptr<SourceUnit>> video_media_index;
    std::vector<SyncSample> sync_samples;
    SyncStepState sync_step;
    FreezeTracker freeze;
    FaultConfig fault;
    PreviewMirror preview;
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
    std::deque<std::pair<int64_t, double>> transport_samples;
    std::deque<std::pair<int64_t, PipelineTimes>> pipeline_samples;
    std::deque<int64_t> presentation_times;
    uint64_t received_video_units = 0;
    uint64_t receiver_connection = 0;
    std::deque<std::pair<int64_t, uint64_t>> byte_samples;
    int64_t last_audio_logged_us = 0;
    std::vector<AlertState> alarms{5};
    std::vector<Json> completed_freezes;
    size_t video_queue_bytes = 0, video_queue_peak_bytes = 0;
    uint64_t video_budget_drops = 0, video_late_drops = 0, timestamp_recovered = 0;
    std::atomic<uint64_t> output_lock_skips{0};
    double conversion_ms = 0, render_work_ms = 0;
    std::optional<AudioClockSample> audio_sample;
    uint64_t sync_segment = 0;
    std::deque<int64_t> unmapped_outputs;
    bool last_video_source_valid = false;
    int64_t last_video_drop_us = 0;
    explicit Chain(Application &a, ChainConfig c);
    ~Chain();
    void start();
    void stop();
    void set_fault(const Json &j);
    Json snapshot();
    void tick();
    void did_present(uint64_t id, int64_t media, int64_t source_time, int64_t observed, uint64_t generation,
                     double render_ms = 0);
    void did_drop(uint64_t id, const std::string &reason, uint64_t generation);
    void discard_locked(uint64_t id, const std::string &reason, uint64_t generation);
    void reset_playback_locked(const std::string &reason);
    void audio_unavailable(uint64_t generation, const std::string &reason);
    void audio_progress(const AudioClockSample &sample);
    void rtp_frame_observed(const std::vector<uint8_t> &access_unit, size_t before, size_t after,
                            uint32_t timestamp);
    void injection_truth(const Json &value);

  private:
    std::thread publisher_, receiver_;
    void publish();
    void receive();
    std::shared_ptr<SourceUnit> match(const std::string &signature, bool video, int64_t pts, int64_t received);
    std::optional<int64_t> video_pts_offset_, audio_pts_offset_;
    int64_t last_video_match_us_ = 0;
    std::shared_ptr<SourceUnit> match_damaged_pts(int64_t pts, int64_t received);
    void observe(const Json &j);
};
} // namespace avqm
