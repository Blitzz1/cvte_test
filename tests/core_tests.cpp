#include "detection/core.hpp"
#include "storage/observations.hpp"
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <thread>
using namespace avqm;
void check(bool v, const char *name) {
    if (!v)
        throw std::runtime_error(name);
}
int main() {
    try {
        check(measured_latency_ms(1000000, 1300000) == 300, "latency units");
        check(!measured_latency_ms(100, 99), "negative latency rejected");
        auto budget = std::make_shared<ByteBudget>(1024);
        auto pixels = ByteLease::reserve(budget, 800);
        auto shared = pixels;
        check(pixels && budget->used() == 800 && !ByteLease::reserve(budget, 300),
              "pixel byte cap before allocation");
        pixels.reset();
        check(budget->used() == 800, "shared image remains charged");
        shared.reset();
        check(budget->used() == 0, "pixel storage last-owner release");
        check(!ByteLease::reserve(budget, std::numeric_limits<size_t>::max()), "budget overflow rejected");
        std::vector<std::thread> producers;
        for (int i = 0; i < 8; ++i)
            producers.emplace_back([&] {
                for (int k = 0; k < 2000; ++k) {
                    auto lease = ByteLease::reserve(budget, 160);
                    if (lease)
                        std::this_thread::yield();
                }
            });
        for (auto &t : producers)
            t.join();
        check(budget->used() == 0 && budget->peak() <= 1024, "concurrent reservations remain bounded");
        AudioClockSample clock{7, 10000000, 1000000, 1020000};
        check(observed_sync_offset(clock, 7, 10000000, 1010000) == 10, "device clock interpolation");
        check(!observed_sync_offset(clock, 8, 10000000, 1010000), "old playback generation rejected");
        check(!observed_sync_offset(clock, 7, 10000000, 999999), "future clock reading rejected");
        check(!observed_sync_offset(clock, 7, 10000000, 1015000), "stale audio clock rejected");
        clock.valid_until_us = 1009000;
        check(!observed_sync_offset(clock, 7, 10000000, 1010000),
              "no extrapolation beyond mapped PCM segment");
        std::vector<SyncSample> segments;
        SyncStepState step;
        for (int i = 0; i < 1800; i++)
            append_sync_segment(segments, {i / 30.0, i / 30.0 * 5}, step);
        check(segments.size() == 1800 && std::abs(fit_drift(segments)->slope_ms_per_s - 5) < 1e-9,
              "gradual drift retains fit segment");
        check(!append_sync_segment(segments, {60, 352}, step) && step.pending,
              "isolated real output jitter is retained for verification");
        check(!append_sync_segment(segments, {60.04, 300.2}, step) && !step.pending && segments.size() > 1,
              "one delayed video frame does not erase continuous drift evidence");
        check(!append_sync_segment(segments, {60.08, 650}, step) && step.pending,
              "fixed step waits for sustained evidence");
        append_sync_segment(segments, {60.14, 650}, step);
        check(append_sync_segment(segments, {60.20, 650}, step) && segments.size() == 1 && !step.pending,
              "confirmed fixed AV jump is not progressive drift");
        check(append_sync_segment(segments, {62, 650}, step) && segments.size() == 1,
              "clock gap resets trend evidence");
        auto dir = std::filesystem::current_path() / "observation-tests" /
                   std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
        ObservationJournal journal(dir, 1024, false);
        size_t accepted = 0;
        for (int i = 0; i < 100; ++i)
            if (journal.append("stream", "{\"frame\":" + std::to_string(i) + "}"))
                ++accepted;
        check(accepted > 0 && accepted < 100 && journal.stats("stream").queued_bytes <= 1024 &&
                  journal.stats("stream").dropped == 100 - accepted,
              "stalled evidence writer is bounded and reports loss");
        check(!journal.append("../escape", "{}"), "observation filename traversal rejected");
        journal.start();
        journal.flush();
        std::ifstream saved(dir / "stream.jsonl");
        size_t saved_count = 0;
        std::string line;
        while (std::getline(saved, line))
            ++saved_count;
        check(saved_count == accepted && journal.stats("stream").written == accepted,
              "accepted evidence flushed without media dependency");
        std::filesystem::create_directory(dir / "blocked.jsonl");
        journal.append("blocked", "{}");
        journal.flush();
        check(journal.stats("blocked").dropped > 0, "disk write failure invalidates evidence completeness");
        journal.stop();
        check(!journal.append("stream", "{}"), "closed journal rejects late producers");
        check(sync_offset_ms(10100000, 10000000) == 100, "audio lead sign");
        std::vector<SyncSample> samples;
        for (int i = 0; i < 60; ++i)
            samples.push_back({double(i), -80 + 2.0 * i});
        auto fit = fit_drift(samples);
        check(fit && std::abs(fit->slope_ms_per_s - 2) < 1e-9, "drift slope");
        check(!fit_drift({{1, 0}, {1, 1}, {1, 2}}), "degenerate regression");
        std::vector<FrameRecord> frames(5);
        frames[0].outcome = Outcome::presented;
        frames[1].outcome = Outcome::transport_lost;
        frames[2].outcome = Outcome::presentation_dropped;
        frames[3].outcome = Outcome::presented;
        auto loss = summarize_loss(frames);
        check(loss.expected == 4 && loss.pending == 1 && loss.missing == 2 && loss.percent == 50 &&
                  loss.consecutive_max == 2,
              "exclusive outcomes and pending denominator");
        check(!summarize_loss({}).percent, "zero denominator unavailable");
        frames[0].damaged = true;
        auto concealed = summarize_loss(frames);
        check(concealed.missing == loss.missing && concealed.reasons.at("presented") == 2,
              "transport-damaged but presented frame is not counted as a missing frame");
        FreezeTracker freeze;
        for (int i = 0; i < 31; ++i)
            freeze.presented(i * 33333);
        check(!freeze.frozen(1050000), "normal frame gap");
        check(freeze.frozen(1300000), "freeze before recovery");
        freeze.presented(1300000);
        check(freeze.completed_count == 1, "single freeze completed");
        check(freeze.threshold_ms() < 190, "freeze must not inflate baseline");
        AlertState alert;
        check(!alert.update(Level::warning, 0, 3000000, 3000000), "debounce begins");
        check(!alert.update(Level::warning, 2999999, 3000000, 3000000), "short excursion");
        check(alert.update(Level::warning, 3000000, 3000000, 3000000).has_value(), "alarm triggered");
        check(!alert.update({}, 4000000, 3000000, 3000000) && alert.level() == Level::warning,
              "missing data is not recovery");
        alert.update(Level::normal, 5000000, 3000000, 3000000);
        check(alert.update(Level::normal, 8000000, 3000000, 3000000).has_value(), "recovery hold");
        RtpSequenceTracker rtp;
        rtp.receive(65534);
        rtp.receive(0);
        rtp.receive(65535);
        rtp.receive(0);
        rtp.receive(2);
        check(rtp.finalize_before(1) == 1, "RTP wrap and true gap");
        check(rtp.duplicates == 1 && rtp.reordered == 1, "RTP duplicates and reorder");
        std::cout << "All detection core cases passed\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
