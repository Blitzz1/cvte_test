#include "detection/core.hpp"
#include <iostream>
#include <stdexcept>
using namespace avqm;
void check(bool v, const char *name) {
    if (!v)
        throw std::runtime_error(name);
}
int main() {
    try {
        check(measured_latency_ms(1000000, 1300000) == 300, "latency units");
        check(!measured_latency_ms(100, 99), "negative latency rejected");
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
