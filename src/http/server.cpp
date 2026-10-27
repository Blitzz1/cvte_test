#include "http/server.hpp"
#include "app/logging.hpp"
#include "app/services.hpp"
#include "http/flv.hpp"
#include "media/chain.hpp"
#include "playback/output.hpp"
#include "storage/store.hpp"
#include <httplib.h>
namespace avqm {
static httplib::Server *active_server = nullptr;
static std::mutex server_mutex;
static void respond(httplib::Response &res, const Json &value, int code = 200) {
    res.status = code;
    res.set_content(value.dump(), "application/json; charset=utf-8");
    res.set_header("Cache-Control", "no-store");
}
void stop_http() {
    std::lock_guard lock(server_mutex);
    if (active_server)
        active_server->stop();
}
void run_http(Application &app, int port) {
    httplib::Server server;
    {
        std::lock_guard lock(server_mutex);
        active_server = &server;
    }
    server.new_task_queue = [] { return new httplib::ThreadPool(16); };
    server.set_payload_max_length(1024 * 1024);
    server.set_read_timeout(5, 0);
    server.set_write_timeout(5, 0);
    server.set_mount_point("/", (app.root / "web").string());
    install_flv_route(server, app);
    server.set_pre_routing_handler([&](const httplib::Request &req, httplib::Response &res) {
        auto host = req.get_header_value("Host");
        if (host != "127.0.0.1:" + std::to_string(port) && host != "localhost:" + std::to_string(port)) {
            respond(res, {{"error", "local host required"}}, 403);
            return httplib::Server::HandlerResponse::Handled;
        }
        if (req.method != "GET" && req.path != "/webhook-demo" &&
            req.get_header_value("X-AVQM-Token") != app.token) {
            respond(res, {{"error", "invalid token"}}, 403);
            return httplib::Server::HandlerResponse::Handled;
        }
        return httplib::Server::HandlerResponse::Unhandled;
    });
    server.set_exception_handler([](const auto &, auto &res, std::exception_ptr ep) {
        std::string s = "request failed";
        try {
            std::rethrow_exception(ep);
        } catch (const std::exception &e) {
            s = e.what();
        }
        respond(res, {{"error", s}}, 400);
    });
    server.Get("/api/v1/health", [&](const auto &, auto &res) {
        respond(res, {{"status", "running"},
                      {"time_ms", utc_ms()},
                      {"token", app.token},
                      {"implementation", "C++ / FFmpeg / native software output"}});
    });
    server.Get("/api/v1/chains", [&](const auto &, auto &res) { respond(res, app.snapshot()); });
    server.Get("/api/v1/config", [&](const auto &, auto &res) {
        std::lock_guard lock(app.config_mutex);
        respond(res, app.config);
    });
    server.Patch("/api/v1/config", [&](const auto &req, auto &res) {
        auto update = Json::parse(req.body);
        std::lock_guard lock(app.config_mutex);
        if (update.contains("revision") && update["revision"] != app.config["revision"]) {
            respond(res, {{"error", "Configuration changed; reload before saving"}}, 409);
            return;
        }
        auto next = app.config;
        next.merge_patch(update);
        auto s = next.value("settings", Json::object());
        auto validated = parse_settings(s);
        next["revision"] = app.config.value("revision", 1) + 1;
        if (!next["chains"].is_array() || next["chains"].size() < 1 || next["chains"].size() > 8)
            throw std::runtime_error("configure 1..8 complete chains");
        std::vector<std::string> ids;
        for (auto &item : next["chains"]) {
            auto id = item.at("id").get<std::string>();
            if (id.empty() ||
                id.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_") !=
                    std::string::npos ||
                std::find(ids.begin(), ids.end(), id) != ids.end())
                throw std::runtime_error("invalid or duplicate chain id");
            ids.push_back(id);
            if (!item.contains("source") || !item.contains("publish_url"))
                throw std::runtime_error("chain source and publish_url required");
        }
        auto path = app.config_file;
        auto temp = path;
        temp += ".tmp";
        {
            std::ofstream f(temp);
            f << next.dump(2);
            if (!f)
                throw std::runtime_error("cannot persist config");
        }
        std::filesystem::rename(temp, path);
        app.config = next;
        app.settings = validated;
        app.video_budget->set_limit(size_t(validated.video_total_mib) * 1024 * 1024);
        Logger::instance().set_retention(validated.retention_days);
        Logger::instance().set_level(next.value("logging", Json::object()).value("level", "info"));
        respond(res, {{"saved", true},
                      {"revision", next["revision"]},
                      {"connection_changes",
                       "restart affected chain; adding/removing chains requires application restart"}});
    });
    server.Post(R"(/api/v1/chains/([^/]+)/(start|stop|restart))", [&](const auto &req, auto &res) {
        auto c = app.find(req.matches[1]);
        if (!c) {
            respond(res, {{"error", "chain not found"}}, 404);
            return;
        }
        std::string op = req.matches[2];
        auto id = app.services->command(c->cfg.id, op);
        respond(res, {{"command_id", id}, {"state", "queued"}}, 202);
    });
    server.Get(R"(/api/v1/commands/([^/]+))", [&](const auto &req, auto &res) {
        auto j = app.services->command_status(req.matches[1]);
        respond(res, j, j.is_null() ? 404 : 200);
    });
    server.Post("/webhook-demo", [&](const auto &req, auto &res) {
        app.store->event("", "webhook_received", Json::parse(req.body));
        respond(res, {{"received", true}});
    });
    server.Post(R"(/api/v1/chains/([^/]+)/fault-injection)", [&](const auto &req, auto &res) {
        auto c = app.find(req.matches[1]);
        if (!c) {
            respond(res, {{"error", "chain not found"}}, 404);
            return;
        }
        auto j = Json::parse(req.body);
        for (auto key : {"presentation_drop_pct", "frame_loss_pct", "packet_loss_pct"})
            if (j.value(key, 0.0) < 0 || j.value(key, 0.0) > 100)
                throw std::runtime_error("probability must be 0..100");
        if (!std::isfinite(j.value("drift_ms_per_s", 0.0)) || std::abs(j.value("drift_ms_per_s", 0.0)) > 10)
            throw std::runtime_error("drift must be -10..10 ms/s");
        for (auto key : {"delay_ms", "audio_delay_ms", "video_delay_ms"})
            if (j.value(key, 0) < 0 || j.value(key, 0) > 3000)
                throw std::runtime_error("delay must be 0..3000 ms");
        if (j.value("packet_loss_pct", 0.0) > 0 && (c->cfg.protocol != "rtsp" || c->cfg.transport != "udp"))
            throw std::runtime_error("RTP datagram injection is only available on the RTSP UDP relay path");
        c->set_fault(j);
        app.event(c->cfg.id, "fault_config", j);
        respond(res, {{"applied", true}});
    });
    server.Get(R"(/api/v1/chains/([^/]+)/metrics)", [&](const auto &req, auto &res) {
        auto c = app.find(req.matches[1]);
        if (!c) {
            respond(res, {{"error", "not found"}}, 404);
            return;
        }
        int64_t to = req.has_param("to") ? std::stoll(req.get_param_value("to")) : utc_ms() + 1;
        int64_t from = req.has_param("from") ? std::stoll(req.get_param_value("from")) : to - 3600000;
        if (from >= to || to - from > 8LL * 86400000)
            throw std::runtime_error("invalid time range");
        respond(res, app.store->history(c->cfg.id, from, to));
    });
    server.Get("/api/v1/events", [&](const auto &req, auto &res) {
        auto chain = req.has_param("chain") ? req.get_param_value("chain") : "";
        auto to = req.has_param("to") ? std::stoll(req.get_param_value("to")) : utc_ms() + 1;
        auto from = req.has_param("from") ? std::stoll(req.get_param_value("from")) : to - 86400000;
        respond(res, app.store->events(chain, from, to));
    });
    server.Post("/api/v1/audio", [&](const auto &req, auto &res) {
        auto j = Json::parse(req.body);
        app.output->select_audio(j.value("index", -1));
        respond(res, {{"selected", j.value("index", -1)}});
    });
    server.Post("/api/v1/shutdown", [&](const auto &, auto &res) {
        app.quit = true;
        respond(res, {{"stopping", true}});
    });
    if (!server.listen("127.0.0.1", port)) {
        std::cerr << "HTTP bind failed: " << port << '\n';
        app.quit = true;
    }
    {
        std::lock_guard lock(server_mutex);
        active_server = nullptr;
    }
}
} // namespace avqm
