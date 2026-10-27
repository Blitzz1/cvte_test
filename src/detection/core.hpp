#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <deque>
#include <map>
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

enum class Outcome {
    pending,
    presented,
    source_skipped,
    send_failed,
    transport_lost,
    decode_failed,
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
};
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
