#include "app/logging.hpp"
#include "app/services.hpp"
#include "app/types.hpp"
#include "http/server.hpp"
#include "media/chain.hpp"
#include "media/rtsp_relay.hpp"
#include "playback/output.hpp"
#include "storage/store.hpp"
#include <httplib.h>
#include <iostream>
#include <random>
extern "C" {
#include <libavformat/avformat.h>
}
namespace avqm {
Application::Application(std::filesystem::path r, std::filesystem::path config_path)
    : root(std::move(r)), config_file(root / config_path) {
    std::ifstream file(config_file);
    if (!file)
        throw std::runtime_error("config/app.json not found");
    file >> config;
    Logger::instance().initialize(root, config.value("logging", Json::object()).value("level", "info"));
    std::random_device rd;
    token = std::to_string(rd()) + std::to_string(rd());
    auto s = config.value("settings", Json::object());
    settings = parse_settings(s);
    Logger::instance().set_retention(settings.retention_days);
    if (!config.contains("revision"))
        config["revision"] = 1;
    store = std::make_unique<Store>(root / "data/quality.db");
    for (auto &j : config.at("chains")) {
        ChainConfig c;
        c.id = j.at("id");
        c.alias = j.value("alias", c.id);
        c.source = j.value("source", std::string{});
        c.protocol = j.value("protocol", "rtsp");
        c.transport = j.value("transport", "tcp");
        c.publish_url = j.at("publish_url");
        c.pull_url = j.value("pull_url", c.publish_url);
        c.enabled = j.value("enabled", true);
        c.push_enabled = j.value("direction", "managed") != "pull";
        c.has_audio = j.value("has_audio", true);
        chains.push_back(std::make_shared<Chain>(*this, c));
    }
    output = std::make_unique<Output>(*this);
    relay = std::make_unique<RtspRelay>(*this);
    relay->start();
    services = std::make_unique<Services>(*this);
}
Application::~Application() {
    quit = true;
    services.reset();
    for (auto &c : chains)
        c->stop();
    relay.reset();
    output.reset();
    chains.clear();
    Logger::instance().stop();
}
Settings Application::settings_copy() {
    std::lock_guard lock(config_mutex);
    return settings;
}
Json Application::snapshot() {
    Json a = Json::array();
    for (auto &c : chains)
        a.push_back(c->snapshot());
    return a;
}
std::shared_ptr<Chain> Application::find(const std::string &id) {
    for (auto &c : chains)
        if (c->cfg.id == id)
            return c;
    return {};
}
void Application::event(std::string chain, std::string type, Json payload) {
    auto id = store->event(chain, type, payload);
    std::cout << type << " " << chain << " " << id << '\n';
    Logger::instance().write(32, type + " chain=" + chain + " event=" + id);
    if (type == "alert") {
        auto url = settings_copy().webhook;
        if (!url.empty()) { // bounded delivery handled by the independent notification worker added after
                            // media validation
            services->notify({{"event_id", id},
                              {"time_ms", utc_ms()},
                              {"chain", chain},
                              {"type", type},
                              {"payload", payload}},
                             url);
        }
    }
}
} // namespace avqm
int main(int argc, char **argv) {
    using namespace avqm;
    try {
        std::filesystem::path root = std::filesystem::current_path();
        int seconds = 0;
        std::filesystem::path config_path = "config/app.json";
        for (int i = 1; i < argc; ++i) {
            if (std::string(argv[i]) == "--root" && i + 1 < argc)
                root = argv[++i];
            else if (std::string(argv[i]) == "--seconds" && i + 1 < argc)
                seconds = std::stoi(argv[++i]);
            else if (std::string(argv[i]) == "--config" && i + 1 < argc)
                config_path = argv[++i];
        }
        avformat_network_init();
        av_log_set_level(AV_LOG_WARNING);
        Application app(root, config_path);
        std::thread http([&] { run_http(app, app.config.value("port", 8080)); });
        for (auto &c : app.chains)
            if (c->cfg.enabled)
                c->start();
        std::thread metrics([&] {
            int64_t next_prune = 0;
            while (!app.quit) {
                auto due = now_us() + 1000000;
                for (auto &c : app.chains)
                    try {
                        c->tick();
                    } catch (const std::exception &e) {
                        std::cerr << "tick: " << e.what() << '\n';
                    }
                if (now_us() >= next_prune) {
                    try {
                        app.store->prune(app.settings_copy().retention_days);
                        auto directory = app.root / "data/observations";
                        auto relative = std::filesystem::weakly_canonical(directory).lexically_relative(
                            std::filesystem::weakly_canonical(app.root));
                        if (!relative.empty() && *relative.begin() != "..") {
                            for (auto &entry : std::filesystem::directory_iterator(directory)) {
                                if (entry.is_regular_file() &&
                                    std::filesystem::file_time_type::clock::now() - entry.last_write_time() >
                                        std::chrono::hours(app.settings_copy().retention_days * 24)) {
                                    std::error_code ignored;
                                    std::filesystem::remove(entry.path(), ignored);
                                }
                            }
                        }
                    } catch (const std::exception &e) {
                        Logger::instance().write(16, e.what());
                    }
                    next_prune = now_us() + 600000000;
                }
                wait_until(due, app.quit);
            }
        });
        std::cout << "Dashboard http://127.0.0.1:" << app.config.value("port", 8080) << "\n";
        std::exception_ptr playback_error;
        try {
            app.output->run(seconds);
        } catch (...) {
            playback_error = std::current_exception();
        }
        app.quit = true;
        metrics.join();
        stop_http();
        http.join();
        if (playback_error)
            std::rethrow_exception(playback_error);
        auto final = app.snapshot();
        std::filesystem::create_directories(root / "artifacts");
        std::ofstream(root / "artifacts/last-run.json") << final.dump(2);
        std::cout << final.dump(2) << '\n';
        return 0;
    } catch (const std::exception &e) {
        std::cerr << "fatal: " << e.what() << '\n';
        return 1;
    }
}
