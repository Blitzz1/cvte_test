#include "detection/core.hpp"
#include <numeric>
namespace avqm {
bool ByteBudget::acquire(size_t bytes) {
    auto current = used_.load();
    while (true) {
        auto limit = limit_.load();
        if (bytes > limit || current > limit - bytes)
            return false;
        if (used_.compare_exchange_weak(current, current + bytes)) {
            auto high = peak_.load();
            while (high < current + bytes && !peak_.compare_exchange_weak(high, current + bytes)) {
            }
            return true;
        }
    }
}
std::shared_ptr<ByteLease> ByteLease::reserve(const std::shared_ptr<ByteBudget> &budget, size_t bytes) {
    if (!budget || !budget->acquire(bytes))
        return {};
    std::unique_ptr<ByteLease> lease;
    try {
        lease.reset(new ByteLease(budget, bytes));
    } catch (...) {
        budget->release(bytes);
        throw;
    }
    return std::shared_ptr<ByteLease>(std::move(lease));
}
std::optional<double> observed_sync_offset(const AudioClockSample &a, uint64_t generation, Microseconds video,
                                           Microseconds observed, Microseconds freshness) {
    if (a.generation != generation || observed < a.observed_us || observed - a.observed_us >= freshness ||
        observed >= a.valid_until_us)
        return {};
    return sync_offset_ms(a.media_us + observed - a.observed_us, video);
}
bool append_sync_segment(std::vector<SyncSample> &values, SyncSample value, SyncStepState &step) {
    bool reset = false;
    if (!values.empty() &&
        (value.seconds <= values.back().seconds || value.seconds - values.back().seconds > 0.5)) {
        values.clear();
        step = {};
        reset = true;
    }
    if (!values.empty()) {
        const auto baseline = step.pending ? step.baseline : values.back().offset_ms;
        const auto delta = value.offset_ms - baseline;
        if (std::abs(delta) <= 50) {
            step = {};
        } else {
            const int direction = delta > 0 ? 1 : -1;
            if (!step.pending || step.direction != direction)
                step = {true, baseline, value.seconds, direction, 1};
            else
                ++step.count;
            if (step.count >= 3 && value.seconds - step.started >= 0.1) {
                values.clear();
                step = {};
                reset = true;
            }
        }
    } else {
        step = {};
    }
    values.push_back(value);
    auto first = std::lower_bound(values.begin(), values.end(), value.seconds - 60,
                                  [](auto &a, double time) { return a.seconds < time; });
    values.erase(values.begin(), first);
    return reset;
}
std::optional<Fit> fit_drift(const std::vector<SyncSample> &s) {
    if (s.size() < 3)
        return {};
    double x = 0, y = 0;
    for (auto a : s) {
        x += a.seconds;
        y += a.offset_ms;
    }
    x /= s.size();
    y /= s.size();
    double xx = 0, xy = 0;
    for (auto a : s) {
        xx += (a.seconds - x) * (a.seconds - x);
        xy += (a.seconds - x) * (a.offset_ms - y);
    }
    if (xx < 1e-12)
        return {};
    double k = xy / xx, b = y - k * x, e = 0;
    for (auto a : s)
        e += std::pow(a.offset_ms - b - k * a.seconds, 2);
    return Fit{k, b, std::sqrt(e / s.size()), s.size()};
}
std::optional<double> measured_latency_ms(Microseconds source, Microseconds presented) {
    if (presented < source)
        return {};
    return (presented - source) / 1000.0;
}
std::optional<double> sync_offset_ms(Microseconds a, Microseconds v) {
    return (a - v) / 1000.0;
}
const char *outcome_name(Outcome v) {
    switch (v) {
    case Outcome::pending:
        return "pending";
    case Outcome::presented:
        return "presented";
    case Outcome::source_skipped:
        return "source_skipped";
    case Outcome::send_failed:
        return "send_failed";
    case Outcome::transport_lost:
        return "transport_lost";
    case Outcome::decode_failed:
        return "decode_failed";
    case Outcome::decode_no_output:
        return "decode_no_output";
    case Outcome::decode_after_input_loss:
        return "decode_after_input_loss";
    case Outcome::damaged_decode_no_output:
        return "damaged_decode_no_output";
    case Outcome::receiver_discarded:
        return "receiver_discarded";
    case Outcome::presentation_dropped:
        return "presentation_dropped";
    default:
        return "unknown_missing";
    }
}
Outcome resolve_missing(const FrameRecord &f, Microseconds now, Microseconds timeout,
                        uint64_t generation, bool ambiguous_output) {
    if (f.outcome != Outcome::pending || now - f.source_us <= timeout)
        return f.outcome;
    // A late-arriving input gets its full decoder wait, even if other output is unmapped.
    if (f.decode_submitted_us && f.decode_generation == generation &&
        (now - *f.decode_submitted_us <= timeout || f.decode_us))
        return Outcome::pending;
    // Unassociated output could be this frame; do not claim confirmed decoder loss.
    if (ambiguous_output)
        return Outcome::unknown_missing;
    if (f.decode_submitted_us && now - *f.decode_submitted_us > timeout &&
        f.decode_generation == generation && !f.decode_us) {
        if (f.damaged)
            return Outcome::damaged_decode_no_output;
        if (f.preceding_input_loss_id)
            return Outcome::decode_after_input_loss;
        return Outcome::decode_no_output;
    }
    return f.damaged ? Outcome::transport_lost : Outcome::unknown_missing;
}
std::optional<PipelineTimes> measured_pipeline_times(const FrameRecord &f) {
    if (!f.receive_us || !f.decode_started_us || !f.decode_submitted_us || !f.decode_us || !f.present_us ||
        f.source_us > *f.receive_us || *f.receive_us > *f.decode_started_us ||
        *f.decode_started_us > *f.decode_submitted_us ||
        *f.decode_submitted_us > *f.decode_us || *f.decode_us > *f.present_us)
        return {};
    return PipelineTimes{(*f.receive_us - f.source_us) / 1000.0,
                         (*f.decode_started_us - *f.receive_us) / 1000.0,
                         (*f.decode_us - *f.decode_started_us) / 1000.0,
                         (*f.decode_us - *f.receive_us) / 1000.0,
                         (*f.present_us - *f.decode_us) / 1000.0,
                         (*f.present_us - *f.receive_us) / 1000.0,
                         (*f.present_us - f.source_us) / 1000.0,
                         (*f.decode_submitted_us - *f.decode_started_us) / 1000.0};
}
LossWindow summarize_loss(const std::vector<FrameRecord> &f) {
    LossWindow w;
    size_t run = 0;
    for (const auto &a : f) {
        if (a.outcome == Outcome::pending) {
            ++w.pending;
            run = 0;
            continue;
        }
        ++w.expected;
        ++w.reasons[outcome_name(a.outcome)];
        if (a.outcome != Outcome::presented) {
            ++w.missing;
            w.consecutive_max = std::max(w.consecutive_max, ++run);
        } else
            run = 0;
    }
    if (w.expected)
        w.percent = 100.0 * w.missing / w.expected;
    return w;
}
double FreezeTracker::threshold_ms() const {
    double d = 1000.0 / 30;
    if (!intervals_.empty())
        d = std::accumulate(intervals_.begin(), intervals_.end(), 0.0) / intervals_.size();
    return std::max(3 * d, d + 150);
}
void FreezeTracker::presented(Microseconds now) {
    if (previous_ && now > *previous_) {
        double gap = (now - *previous_) / 1000.0;
        if (gap >= threshold_ms()) {
            ++completed_count;
            completed_duration_ms += gap;
        } else {
            intervals_.push_back(gap);
            if (intervals_.size() > 30)
                intervals_.pop_front();
        }
    }
    previous_ = now;
}
double FreezeTracker::stalled_ms(Microseconds now) const {
    return previous_ ? std::max(0.0, (now - *previous_) / 1000.0) : 0;
}
bool FreezeTracker::frozen(Microseconds now) const {
    return previous_ && stalled_ms(now) >= threshold_ms();
}
void FreezeTracker::reset() {
    previous_.reset();
    intervals_.clear();
    completed_count = 0;
    completed_duration_ms = 0;
}
const char *level_name(Level l) {
    return l == Level::critical ? "critical" : l == Level::warning ? "warning" : "normal";
}
void AlertState::reset() {
    current_ = candidate_ = Level::normal;
    since_.reset();
}
std::optional<AlertChange> AlertState::update(std::optional<Level> target, Microseconds now,
                                              Microseconds hold, Microseconds recovery) {
    if (!target) {
        since_.reset();
        return {};
    }
    if (*target == current_) {
        since_.reset();
        return {};
    }
    if (!since_ || candidate_ != *target) {
        candidate_ = *target;
        since_ = now;
    }
    if (now - *since_ < (*target == Level::normal ? recovery : hold))
        return {};
    auto old = current_;
    current_ = *target;
    since_.reset();
    return AlertChange{old, current_,
                       current_ == Level::normal ? "recovered"
                       : old == Level::normal    ? "occurred"
                       : current_ > old          ? "escalated"
                                                 : "deescalated"};
}
void RtpSequenceTracker::receive(uint16_t seq) {
    if (!initialized_) {
        initialized_ = true;
        high_ = next_ = seq;
    }
    int64_t candidate = (high_ & ~int64_t(65535)) | seq;
    if (candidate - high_ > 32768)
        candidate -= 65536;
    else if (high_ - candidate > 32768)
        candidate += 65536;
    if (candidate < next_) {
        ++reordered;
        return;
    }
    if (pending_.contains(candidate)) {
        ++duplicates;
        return;
    }
    if (candidate < high_)
        ++reordered;
    high_ = std::max(high_, candidate);
    pending_[candidate] = true;
    ++received;
}
size_t RtpSequenceTracker::finalize_before(uint32_t distance) {
    if (!initialized_)
        return 0;
    size_t n = 0;
    while (next_ <= high_ - distance) {
        if (!pending_.erase(next_))
            ++n;
        ++next_;
    }
    lost += n;
    return n;
}
} // namespace avqm
