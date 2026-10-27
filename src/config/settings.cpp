#include "app/types.hpp"
namespace avqm {
Settings parse_settings(const Json &j) {
    Settings s;
    s.delay_warning_ms = j.value("delay_warning_ms", 400.0);
    s.delay_critical_ms = j.value("delay_critical_ms", 1000.0);
    s.loss_warning_pct = j.value("loss_warning_pct", 1.0);
    s.loss_critical_pct = j.value("loss_critical_pct", 5.0);
    s.av_lead_warning_ms = j.value("av_lead_warning_ms", 45.0);
    s.av_lag_warning_ms = j.value("av_lag_warning_ms", 125.0);
    s.av_lead_critical_ms = j.value("av_lead_critical_ms", 90.0);
    s.av_lag_critical_ms = j.value("av_lag_critical_ms", 185.0);
    s.drift_warning_ms_per_s = j.value("drift_warning_ms_per_s", 1.0);
    s.freeze_critical_ms = j.value("freeze_critical_ms", 1000);
    s.drift_hold_ms = j.value("drift_hold_ms", 30000);
    s.hold_ms = j.value("hold_ms", 3000);
    s.recovery_ms = j.value("recovery_ms", 3000);
    s.retention_days = j.value("retention_days", 7);
    s.resolution_timeout_ms = j.value("resolution_timeout_ms", 5000);
    s.webhook = j.value("webhook", std::string{});
    auto pair = [](double a, double b, const char *name) {
        if (!std::isfinite(a) || !std::isfinite(b) || a < 0 || b <= a)
            throw std::runtime_error(std::string(name) + ": critical must exceed non-negative warning");
    };
    pair(s.delay_warning_ms, s.delay_critical_ms, "delay");
    pair(s.loss_warning_pct, s.loss_critical_pct, "loss");
    pair(s.av_lead_warning_ms, s.av_lead_critical_ms, "audio lead");
    pair(s.av_lag_warning_ms, s.av_lag_critical_ms, "audio lag");
    if (s.loss_critical_pct > 100 || s.hold_ms < 0 || s.hold_ms > 60000 || s.recovery_ms < 0 ||
        s.recovery_ms > 60000 || s.retention_days < 1 || s.retention_days > 365 ||
        s.resolution_timeout_ms < 1000 || s.resolution_timeout_ms > 60000 || s.freeze_critical_ms < 1 ||
        s.freeze_critical_ms > 60000 || s.drift_hold_ms < 0 || !std::isfinite(s.drift_warning_ms_per_s) ||
        s.drift_warning_ms_per_s <= 0)
        throw std::runtime_error("settings value out of supported range");
    if (!s.webhook.empty() && !s.webhook.starts_with("http://"))
        throw std::runtime_error("This build requires an HTTP webhook URL");
    return s;
}
} // namespace avqm
