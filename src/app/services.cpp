#include "app/services.hpp"
#include "media/chain.hpp"
#include "storage/store.hpp"
#include <httplib.h>
#include <regex>
namespace avqm {
Services::Services(Application &a) : app_(a) {
    control_ = std::thread([this] { control_loop(); });
    notifier_ = std::thread([this] { notification_loop(); });
}
Services::~Services() {
    done_ = true;
    if (control_.joinable())
        control_.join();
    if (notifier_.joinable())
        notifier_.join();
}
std::string Services::command(const std::string &chain, const std::string &action) {
    std::lock_guard lock(mutex_);
    auto id = std::to_string(utc_ms()) + "-cmd-" + std::to_string(++sequence_);
    if (commands_.size() >= 32)
        throw std::runtime_error("control queue full");
    Json job = {{"id", id}, {"chain", chain}, {"action", action}, {"state", "queued"}};
    commands_.push_back(job);
    statuses_[id] = job;
    return id;
}
Json Services::command_status(const std::string &id) {
    std::lock_guard lock(mutex_);
    auto it = statuses_.find(id);
    return it == statuses_.end() ? Json(nullptr) : it->second;
}
void Services::notify(const Json &p, const std::string &url) {
    std::lock_guard lock(mutex_);
    if (notifications_.size() >= 1000) {
        app_.store->event("", "webhook_queue_full", {{"event_id", p.value("event_id", "")}});
        return;
    }
    notifications_.push_back({{"payload", p}, {"url", url}});
}
void Services::control_loop() {
    while (!done_ && !app_.quit) {
        Json job;
        {
            std::lock_guard lock(mutex_);
            if (!commands_.empty()) {
                job = commands_.front();
                commands_.pop_front();
                statuses_[job["id"].get<std::string>()]["state"] = "running";
            }
        }
        if (job.is_null()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            continue;
        }
        try {
            auto c = app_.find(job["chain"]);
            if (!c)
                throw std::runtime_error("chain not found");
            std::string action = job["action"];
            if (action == "close")
                app_.close_chain(c);
            if (action == "stop" || action == "restart")
                c->stop();
            if (action == "start" || action == "restart") {
                {
                    std::lock_guard config_lock(app_.config_mutex);
                    for (auto &j : app_.config["chains"])
                        if (j["id"] == c->cfg.id) {
                            std::lock_guard chain_lock(c->mutex);
                            c->cfg.alias = j.value("alias", c->cfg.id);
                            c->cfg.source = j.value("source", std::string{});
                            c->cfg.push_enabled = j.value("direction", "managed") != "pull";
                            c->cfg.has_audio = j.value("has_audio", true);
                            c->cfg.protocol = j.value("protocol", "rtsp");
                            c->cfg.transport = j.value("transport", "tcp");
                            c->cfg.publish_url = j.at("publish_url");
                            c->cfg.pull_url = j.value("pull_url", c->cfg.publish_url);
                        }
                }
                c->start();
            }
            job["state"] = "completed";
            app_.event(c->cfg.id, "control", job);
        } catch (const std::exception &e) {
            job["state"] = "failed";
            job["error"] = e.what();
        }
        std::lock_guard lock(mutex_);
        statuses_[job["id"].get<std::string>()] = job;
        while (statuses_.size() > 200)
            statuses_.erase(statuses_.begin());
    }
}
void Services::notification_loop() {
    while (!done_ && !app_.quit) {
        Json job;
        {
            std::lock_guard lock(mutex_);
            if (!notifications_.empty()) {
                job = notifications_.front();
                notifications_.pop_front();
            }
        }
        if (job.is_null()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }
        std::string url = job["url"];
        std::smatch m;
        if (!std::regex_match(url, m, std::regex("(http://[^/]+)(/.*)?"))) {
            app_.store->event("", "webhook_failed",
                              {{"event_id", job["payload"]["event_id"]},
                               {"reason", "plain HTTP URL required in this build"}});
            continue;
        }
        bool delivered = false;
        for (int attempt = 1; attempt <= 3 && !done_ && !app_.quit; ++attempt) {
            httplib::Client client(m[1].str());
            client.set_connection_timeout(3, 0);
            client.set_read_timeout(3, 0);
            client.set_write_timeout(3, 0);
            std::string path = m[2].matched ? m[2].str() : "/";
            auto result = client.Post(path.c_str(), job["payload"].dump(), "application/json");
            delivered = result && result->status >= 200 && result->status < 300;
            app_.store->event("", "webhook_delivery",
                              {{"event_id", job["payload"]["event_id"]},
                               {"attempt", attempt},
                               {"delivered", delivered},
                               {"http_status", result ? result->status : 0}});
            if (delivered)
                break;
            wait_until(now_us() + (1LL << (attempt - 1)) * 1000000, done_);
        }
        if (!delivered)
            app_.store->event("", "webhook_failed", {{"event_id", job["payload"]["event_id"]}});
    }
}
} // namespace avqm
