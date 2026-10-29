#include "app/logging.hpp"
#include "app/services.hpp"
#include "app/types.hpp"
#include "http/server.hpp"
#include "media/chain.hpp"
#include "media/rtsp_relay.hpp"
#include "playback/output.hpp"
#include "storage/observations.hpp"
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
    video_budget = std::make_shared<ByteBudget>(size_t(settings.video_total_mib) * 1024 * 1024);
    journal = std::make_unique<ObservationJournal>(root / "data/observations");
    Logger::instance().set_retention(settings.retention_days);
    if (!config.contains("revision"))
        config["revision"] = 1;
    store = std::make_unique<Store>(root / "data/quality.db");
    for (auto &j : config.at("chains")) {
        if (!j.contains("pull_url")) j["pull_url"] = j.at("publish_url");
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
    journal.reset();
    Logger::instance().stop();
}
Settings Application::settings_copy() {
    std::lock_guard lock(config_mutex);
    return settings;
}
Json Application::snapshot() {
    Json a = Json::array();
    for (auto &c : chains_copy())
        a.push_back(c->snapshot());
    return a;
}
std::shared_ptr<Chain> Application::find(const std::string &id) {
    for (auto &c : chains_copy())
        if (c->cfg.id == id)
            return c;
    return {};
}
std::vector<std::shared_ptr<Chain>> Application::chains_copy() const {
    std::lock_guard lock(chains_mutex);
    return chains;
}
std::shared_ptr<Chain> Application::add_chain(const Json &request) {
    std::lock_guard config_lock(config_mutex);
    std::lock_guard list_lock(chains_mutex);
    if (chains.size() >= 8 || config["chains"].size() >= 8)
        throw std::runtime_error("最多支持8条链路");
    auto protocol = request.value("protocol", "rtsp");
    auto transport = request.value("transport", "tcp");
    if ((protocol != "rtsp" && protocol != "rtmp") ||
        (protocol == "rtsp" && transport != "tcp" && transport != "udp"))
        throw std::runtime_error("请选择RTSP TCP、RTSP UDP或RTMP");
    auto source = request.value("source", "data/media/sample-3m.mp4");
    auto source_path = std::filesystem::path(source);
    if (!std::filesystem::is_regular_file(source_path.is_absolute() ? source_path : root / source_path))
        throw std::runtime_error("素材文件不存在");
    int index = config.value("next_chain_index", 1);
    for (;; ++index) {
        auto id = "chain-" + std::to_string(index);
        auto path = "/chain" + std::to_string(index);
        bool used = false;
        for (auto &j : config["chains"])
            if (j.at("id") == id || j.value("publish_url", "").ends_with(path)) used = true;
        for (auto &current : chains)
            if (current->cfg.id == id || current->cfg.publish_url.ends_with(path)) used = true;
        if (!used) break;
    }
    ChainConfig c;
    c.id = "chain-" + std::to_string(index);
    c.alias = request.value("alias", "链路 " + std::to_string(index));
    if (c.alias.empty() || c.alias.size() > 160) throw std::runtime_error("链路名称无效");
    c.source = source;
    c.protocol = protocol;
    c.transport = transport;
    c.has_audio = request.value("has_audio", true);
    auto path = "/chain" + std::to_string(index);
    c.publish_url = (protocol == "rtmp" ? "rtmp://127.0.0.1:1935" : "rtsp://127.0.0.1:8554") + path;
    c.pull_url = protocol == "rtsp" && transport == "udp" ? "rtsp://127.0.0.1:8555" + path : c.publish_url;
    auto chain = std::make_shared<Chain>(*this, c);
    Json item = {{"id", c.id}, {"alias", c.alias}, {"enabled", true}, {"source", c.source},
                 {"protocol", protocol}, {"transport", transport}, {"publish_url", c.publish_url},
                 {"pull_url", c.pull_url}, {"has_audio", c.has_audio}};
    auto next = config;
    next["chains"].push_back(item);
    next["next_chain_index"] = index + 1;
    next["revision"] = config.value("revision", 1) + 1;
    auto temp = config_file;
    temp += ".tmp";
    { std::ofstream file(temp); file << next.dump(2); if (!file) throw std::runtime_error("无法保存配置"); }
    // Reserve before persisting, so the append cannot fail after the file has been committed.
    chains.reserve(8);
    std::filesystem::rename(temp, config_file);
    config = std::move(next);
    chains.push_back(chain);
    return chain;
}
void Application::close_chain(const std::shared_ptr<Chain> &chain) {
    // Never hold registry/config locks while waiting for media threads to exit.
    chain->stop();
    std::lock_guard config_lock(config_mutex);
    std::lock_guard list_lock(chains_mutex);
    auto found = std::find(chains.begin(), chains.end(), chain);
    if (found == chains.end()) return;
    auto next = config;
    // Do not reuse closed IDs/paths: queued requests and historical samples must
    // never accidentally refer to a newly created chain with the same identity.
    int next_index = next.value("next_chain_index", 1);
    for (auto &j : next["chains"]) {
        auto url = j.value("publish_url", "");
        auto marker = url.rfind("/chain");
        if (marker != std::string::npos) {
            auto suffix = url.substr(marker + 6);
            if (!suffix.empty() && suffix.size() < 8 &&
                suffix.find_first_not_of("0123456789") == std::string::npos)
                next_index = std::max(next_index, std::stoi(suffix) + 1);
        }
    }
    next["next_chain_index"] = next_index;
    auto &items = next["chains"];
    for (auto it = items.begin(); it != items.end();)
        if (it->at("id") == chain->cfg.id) it = items.erase(it); else ++it;
    next["revision"] = config.value("revision", 1) + 1;
    auto temp = config_file;
    temp += ".tmp";
    { std::ofstream file(temp); file << next.dump(2); if (!file) throw std::runtime_error("无法保存关闭页面配置，链路已停止"); }
    std::filesystem::rename(temp, config_file);
    config = std::move(next);
    output->forget_chain(chain);
    chains.erase(found);
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
        for (auto &c : app.chains_copy())
            if (c->cfg.enabled)
                c->start();
        std::thread metrics([&] {
            int64_t next_prune = 0;
            while (!app.quit) {
                auto due = now_us() + 1000000;
                for (auto &c : app.chains_copy())
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
