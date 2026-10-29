#pragma once
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace avqm {
using Microseconds = int64_t;
struct SyncSample {
    double seconds;
    double offset_ms;
};
struct Fit {
    double slope_ms_per_s;
    double intercept_ms;
    double residual_ms;
    size_t count;
};
std::optional<Fit> fit_drift(const std::vector<SyncSample> &samples);
std::optional<double> measured_latency_ms(Microseconds source, Microseconds presented);
std::optional<double> sync_offset_ms(Microseconds audio_source, Microseconds video_source);

// Pixel storage is charged before allocation and released with its last owner.
class ByteBudget {
  public:
    explicit ByteBudget(size_t limit) : limit_(limit) {}
    bool acquire(size_t bytes);
    void release(size_t bytes) {
        used_.fetch_sub(bytes);
    }
    void set_limit(size_t bytes) {
        limit_ = bytes;
    }
    size_t used() const {
        return used_;
    }
    size_t peak() const {
        return peak_;
    }
    size_t limit() const {
        return limit_;
    }

  private:
    std::atomic<size_t> used_{0}, peak_{0}, limit_;
};
class ByteLease {
  public:
    static std::shared_ptr<ByteLease> reserve(const std::shared_ptr<ByteBudget> &budget, size_t bytes);
    ~ByteLease() {
        budget_->release(bytes_);
    }
    ByteLease(const ByteLease &) = delete;
    ByteLease &operator=(const ByteLease &) = delete;

  private:
    ByteLease(std::shared_ptr<ByteBudget> budget, size_t bytes) : budget_(std::move(budget)), bytes_(bytes) {}
    std::shared_ptr<ByteBudget> budget_;
    size_t bytes_;
};
struct AudioClockSample {
    uint64_t generation{};
    Microseconds media_us{}, observed_us{}, valid_until_us{};
    uint64_t device_position{}, device_frequency{}, pcm_begin_frame{};
    Microseconds pcm_begin_media_us{};
};
std::optional<double> observed_sync_offset(const AudioClockSample &audio, uint64_t generation,
                                           Microseconds video_media, Microseconds observed,
                                           Microseconds freshness = 15000);
struct SyncStepState {
    bool pending{};
    double baseline{}, started{};
    int direction{};
    size_t count{};
};
// Keep transient raw jitter; only a persistent step, gap or reverse time cuts the fit.
bool append_sync_segment(std::vector<SyncSample> &samples, SyncSample value, SyncStepState &step);

enum class Outcome {
    pending,
    presented,
    source_skipped,
    send_failed,
    transport_lost,
    decode_failed,
    decode_no_output,
    decode_after_input_loss,
    damaged_decode_no_output,
    receiver_discarded,
    presentation_dropped,
    unknown_missing
};
const char *outcome_name(Outcome value);
struct FrameRecord {
    uint64_t id{};
    Microseconds source_us{}, media_us{};
    Outcome outcome{Outcome::pending};
    std::optional<Microseconds> receive_us, decode_us, present_us;
    bool damaged{};
    std::optional<Microseconds> decode_submitted_us;
    std::optional<Microseconds> decode_started_us;
    uint64_t decode_generation{}, preceding_input_loss_id{};
};
// Stage attribution from observations, never from configured injection probability.
Outcome resolve_missing(const FrameRecord &frame, Microseconds now, Microseconds timeout,
                        uint64_t generation, bool ambiguous_output);
struct PipelineTimes {
    double transport_ms{}, receive_to_submit_ms{}, decoder_ms{}, decode_ms{}, post_decode_ms{},
           receiver_processing_ms{}, total_ms{}, submit_call_ms{};
};
std::optional<PipelineTimes> measured_pipeline_times(const FrameRecord &frame);
struct LossWindow {
    size_t expected{}, pending{}, missing{}, consecutive_max{};
    std::map<std::string, size_t> reasons;
    std::optional<double> percent;
};
LossWindow summarize_loss(const std::vector<FrameRecord> &frames);

class FreezeTracker {
  public:
    void presented(Microseconds now);
    double threshold_ms() const;
    double stalled_ms(Microseconds now) const;
    bool frozen(Microseconds now) const;
    void reset();
    uint64_t completed_count{};
    double completed_duration_ms{};

  private:
    std::optional<Microseconds> previous_;
    std::deque<double> intervals_;
};

enum class Level { normal, warning, critical };
const char *level_name(Level level);
struct AlertChange {
    Level before;
    Level after;
    std::string transition;
};
class AlertState {
  public:
    std::optional<AlertChange> update(std::optional<Level> target, Microseconds now, Microseconds hold,
                                      Microseconds recovery);
    Level level() const {
        return current_;
    }
    void reset();

  private:
    Level current_{Level::normal}, candidate_{Level::normal};
    std::optional<Microseconds> since_;
};

// Extends 16-bit RTP sequence numbers and delays loss confirmation for reordering.
class RtpSequenceTracker {
  public:
    void receive(uint16_t seq);
    size_t finalize_before(uint32_t reorder_distance);
    uint64_t duplicates{}, reordered{}, lost{}, received{};

  private:
    bool initialized_{};
    int64_t high_{}, next_{};
    std::map<int64_t, bool> pending_;
};
} // namespace avqm
