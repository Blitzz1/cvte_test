#include "media/rtsp_relay.hpp"
#include "media/chain.hpp"
#include <regex>
#include <winsock2.h>
#include <ws2tcpip.h>
namespace avqm {
namespace {
sockaddr_in local_address(uint16_t port) {
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = htons(port);
    return a;
}
bool send_all(SOCKET s, const std::string &value) {
    size_t off = 0;
    while (off < value.size()) {
        int n = send(s, value.data() + off, int(value.size() - off), 0);
        if (n <= 0)
            return false;
        off += n;
    }
    return true;
}
std::string read_message(SOCKET s) {
    std::string data;
    char buffer[8192];
    size_t total = std::string::npos;
    while (data.size() < 262144) {
        int n = recv(s, buffer, sizeof(buffer), 0);
        if (n < 0 && WSAGetLastError() == WSAETIMEDOUT && data.empty())
            return "#timeout";
        if (n <= 0)
            return {};
        data.append(buffer, n);
        auto split = data.find("\r\n\r\n");
        if (split != std::string::npos) {
            std::smatch m;
            size_t len = 0;
            auto headers = data.substr(0, split);
            if (std::regex_search(headers, m, std::regex("Content-Length: *([0-9]+)", std::regex::icase)))
                len = std::stoul(m[1]);
            total = split + 4 + len;
        }
        if (total != std::string::npos && data.size() >= total)
            return data;
    }
    return {};
}
std::string replace_all(std::string value, const std::string &from, const std::string &to) {
    size_t pos = 0;
    while ((pos = value.find(from, pos)) != std::string::npos) {
        value.replace(pos, from.size(), to);
        pos += to.size();
    }
    return value;
}
uint16_t be16(const uint8_t *p) {
    return uint16_t(p[0]) * 256 + p[1];
}
uint32_t be32(const uint8_t *p) {
    return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
}
} // namespace
struct RtspRelay::Impl {
    Application &app;
    int port;
    std::atomic<bool> stop_flag{false};
    SOCKET listener = INVALID_SOCKET;
    std::thread accept_thread;
    std::vector<std::thread> clients;
    explicit Impl(Application &a, int p) : app(a), port(p) {}
    struct Track {
        Impl &parent;
        std::shared_ptr<Chain> chain;
        bool video{};
        SOCKET rtp = INVALID_SOCKET, rtcp = INVALID_SOCKET;
        uint16_t original_client{}, relay_port{}, server_port{};
        std::atomic<bool> stopped{false};
        std::thread worker;
        RtpSequenceTracker sequence;
        std::mt19937 rng{1};
        uint32_t seed = 1, current_timestamp = 0;
        bool has_frame = false;
        int64_t last_packet_us = 0;
        size_t before_count = 0, after_count = 0;
        std::vector<uint8_t> access_unit;
        bool fu_open = false;
        uint64_t packets_in = 0;
        Track(Impl &p, std::shared_ptr<Chain> c, bool v, uint16_t client)
            : parent(p), chain(std::move(c)), video(v), original_client(client) {
            for (int tries = 0; tries < 100; ++tries) {
                uint16_t candidate = uint16_t(20000 + (std::random_device{}() % 20000) * 2 % 40000);
                rtp = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
                rtcp = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
                int bytes = 4 * 1024 * 1024;
                setsockopt(rtp, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<char *>(&bytes), sizeof(bytes));
                auto a = local_address(candidate), b = local_address(candidate + 1);
                if (bind(rtp, reinterpret_cast<sockaddr *>(&a), sizeof(a)) == 0 &&
                    bind(rtcp, reinterpret_cast<sockaddr *>(&b), sizeof(b)) == 0) {
                    relay_port = candidate;
                    break;
                }
                closesocket(rtp);
                closesocket(rtcp);
                rtp = rtcp = INVALID_SOCKET;
            }
            if (!relay_port)
                throw std::runtime_error("RTP relay bind failed");
        }
        ~Track() {
            stopped = true;
            if (worker.joinable())
                worker.join();
            if (rtp != INVALID_SOCKET)
                closesocket(rtp);
            if (rtcp != INVALID_SOCKET)
                closesocket(rtcp);
        }
        void nal(const uint8_t *p, size_t size) {
            if (!size)
                return;
            access_unit.insert(access_unit.end(), {0, 0, 0, 1});
            access_unit.insert(access_unit.end(), p, p + size);
        }
        void assemble(const uint8_t *p, size_t size) {
            if (!size)
                return;
            int type = p[0] & 31;
            if (type >= 1 && type <= 23)
                nal(p, size);
            else if (type == 24) {
                size_t pos = 1;
                while (pos + 2 <= size) {
                    size_t n = be16(p + pos);
                    pos += 2;
                    if (pos + n > size)
                        break;
                    nal(p + pos, n);
                    pos += n;
                }
            } else if (type == 28 && size >= 2) {
                if (p[1] & 0x80) {
                    uint8_t header = (p[0] & 0xe0) | (p[1] & 31);
                    nal(&header, 1);
                    fu_open = true;
                }
                if (fu_open)
                    access_unit.insert(access_unit.end(), p + 2, p + size);
                if (p[1] & 0x40)
                    fu_open = false;
            }
        }
        void finish(bool marker_seen = false) {
            if (video && has_frame && !access_unit.empty())
                chain->rtp_frame_observed(access_unit, before_count, after_count, current_timestamp,
                                          last_packet_us, marker_seen);
            access_unit.clear();
            before_count = after_count = 0;
            has_frame = false;
            fu_open = false;
        }
        void run() {
            while (!stopped && !parent.stop_flag && !parent.app.quit) {
                fd_set set;
                FD_ZERO(&set);
                FD_SET(rtp, &set);
                FD_SET(rtcp, &set);
                timeval timeout{0, 100000};
                if (select(0, &set, nullptr, nullptr, &timeout) <= 0)
                    continue;
                for (int channel = 0; channel < 2; ++channel) {
                    SOCKET s = channel ? rtcp : rtp;
                    if (!FD_ISSET(s, &set))
                        continue;
                    uint8_t buffer[65536];
                    sockaddr_in from{};
                    int length = sizeof(from);
                    int n = recvfrom(s, reinterpret_cast<char *>(buffer), sizeof(buffer), 0,
                                     reinterpret_cast<sockaddr *>(&from), &length);
                    const auto packet_observed = now_us();
                    if (n <= 0)
                        continue;
                    bool from_client = ntohs(from.sin_port) == original_client + channel;
                    auto target = local_address(from_client ? uint16_t(server_port + channel)
                                                            : uint16_t(original_client + channel));
                    if (from_client || channel) {
                        sendto(s, reinterpret_cast<char *>(buffer), n, 0,
                               reinterpret_cast<sockaddr *>(&target), sizeof(target));
                        continue;
                    }
                    if (n < 12 || (buffer[0] >> 6) != 2)
                        continue;
                    size_t header = 12 + 4 * (buffer[0] & 15);
                    if (header > size_t(n))
                        continue;
                    if (buffer[0] & 16) {
                        if (header + 4 > size_t(n))
                            continue;
                        header += 4 + 4 * be16(buffer + header + 2);
                    }
                    size_t end = n;
                    if (buffer[0] & 32) {
                        if (buffer[n - 1] > end)
                            continue;
                        end -= buffer[n - 1];
                    }
                    if (header > end)
                        continue;
                    uint16_t seq = be16(buffer + 2);
                    uint32_t stamp = be32(buffer + 4);
                    bool marker = (buffer[1] & 128) != 0;
                    if (video) {
                        if (has_frame && stamp != current_timestamp)
                            finish();
                        has_frame = true;
                        current_timestamp = stamp;
                        last_packet_us = packet_observed;
                        ++before_count;
                        assemble(buffer + header, end - header);
                    }
                    FaultConfig fault;
                    {
                        std::lock_guard lock(chain->mutex);
                        fault = chain->fault;
                    }
                    if (seed != fault.seed) {
                        seed = fault.seed;
                        rng.seed(seed);
                    }
                    ++packets_in;
                    // The detector below sees actual packets emerging from this gate, not its random
                    // decisions.
                    bool suppress = video && fault.packet_loss_pct > 0 &&
                                    std::uniform_real_distribution<>(0, 100)(rng) < fault.packet_loss_pct;
                    if (suppress)
                        chain->injection_truth({{"action", "rtp_datagram_suppressed"},
                                                {"sequence", seq},
                                                {"rtp_timestamp", stamp},
                                                {"mono_us", now_us()},
                                                {"bytes", n}});
                    if (!suppress) {
                        int written = sendto(s, reinterpret_cast<char *>(buffer), n, 0,
                                             reinterpret_cast<sockaddr *>(&target), sizeof(target));
                        if (written == n) {
                            sequence.receive(seq);
                            if (video)
                                ++after_count;
                        }
                    }
                    if (video) {
                        auto loss = sequence.finalize_before(64);
                        std::lock_guard lock(chain->mutex);
                        chain->rtp_packets_received = sequence.received;
                        chain->rtp_packets_lost = sequence.lost;
                        chain->rtp_packets_duplicates = sequence.duplicates;
                        (void)loss;
                    }
                    if (marker && video)
                        finish(true);
                }
            }
            finish();
        }
    };
    void handle(SOCKET client) {
        SOCKET upstream = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        auto server = local_address(8554);
        DWORD timeout = 1000;
        setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<char *>(&timeout), sizeof(timeout));
        setsockopt(upstream, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<char *>(&timeout), sizeof(timeout));
        if (connect(upstream, reinterpret_cast<sockaddr *>(&server), sizeof(server)) != 0) {
            closesocket(client);
            closesocket(upstream);
            return;
        }
        std::vector<std::unique_ptr<Track>> tracks;
        std::shared_ptr<Chain> chain;
        std::string video_control = "trackID=0";
        try {
            while (!stop_flag && !app.quit) {
                auto request = read_message(client);
                if (request == "#timeout")
                    continue;
                if (request.empty())
                    break;
                if (!chain) {
                    for (auto &c : app.chains_copy()) {
                        std::lock_guard chain_lock(c->mutex);
                        auto split = c->cfg.publish_url.find('/', 7);
                        if (split == std::string::npos)
                            continue;
                        auto path = c->cfg.publish_url.substr(split);
                        if (request.find(path) != std::string::npos) {
                            chain = c;
                            break;
                        }
                    }
                }
                auto forwarded = replace_all(request, ":" + std::to_string(port) + "/", ":8554/");
                std::unique_ptr<Track> track;
                std::smatch ports;
                if (request.starts_with("SETUP") && chain &&
                    std::regex_search(request, ports, std::regex("client_port=([0-9]+)-([0-9]+)"))) {
                    bool video = request.find(video_control) != std::string::npos;
                    track = std::make_unique<Track>(*this, chain, video, uint16_t(std::stoul(ports[1])));
                    forwarded = std::regex_replace(forwarded, std::regex("client_port=[0-9]+-[0-9]+"),
                                                   "client_port=" + std::to_string(track->relay_port) + "-" +
                                                       std::to_string(track->relay_port + 1));
                }
                if (!send_all(upstream, forwarded))
                    break;
                auto response = read_message(upstream);
                if (response.empty())
                    break;
                if (request.starts_with("DESCRIBE")) {
                    std::smatch control;
                    if (std::regex_search(response, control,
                                          std::regex("m=video[^\\x00]*?a=control:([^\\r\\n]+)")))
                        video_control = control[1];
                }
                if (track) {
                    std::smatch sp;
                    if (!std::regex_search(response, sp, std::regex("server_port=([0-9]+)-([0-9]+)")))
                        throw std::runtime_error("RTSP server did not return UDP ports");
                    track->server_port = uint16_t(std::stoul(sp[1]));
                    response = std::regex_replace(response, std::regex("client_port=[0-9]+-[0-9]+"),
                                                  "client_port=" + std::to_string(track->original_client) +
                                                      "-" + std::to_string(track->original_client + 1));
                    response = std::regex_replace(response, std::regex("server_port=[0-9]+-[0-9]+"),
                                                  "server_port=" + std::to_string(track->relay_port) + "-" +
                                                      std::to_string(track->relay_port + 1));
                    auto *raw = track.get();
                    track->worker = std::thread([raw] { raw->run(); });
                    tracks.push_back(std::move(track));
                }
                response = replace_all(response, ":8554/", ":" + std::to_string(port) + "/");
                if (!send_all(client, response))
                    break;
            }
        } catch (const std::exception &e) {
            app.event(chain ? chain->cfg.id : "relay", "relay_error", {{"message", e.what()}});
        }
        tracks.clear();
        closesocket(client);
        closesocket(upstream);
    }
    void run() {
        while (!stop_flag && !app.quit) {
            fd_set set;
            FD_ZERO(&set);
            FD_SET(listener, &set);
            timeval tv{0, 100000};
            if (select(0, &set, nullptr, nullptr, &tv) <= 0)
                continue;
            auto client = accept(listener, nullptr, nullptr);
            if (client != INVALID_SOCKET)
                clients.emplace_back([this, client] { handle(client); });
        }
    }
};
RtspRelay::RtspRelay(Application &a, int port) : p_(std::make_unique<Impl>(a, port)) {}
RtspRelay::~RtspRelay() {
    stop();
}
void RtspRelay::start() {
    WSADATA data;
    WSAStartup(MAKEWORD(2, 2), &data);
    p_->listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    auto a = local_address(uint16_t(p_->port));
    if (bind(p_->listener, reinterpret_cast<sockaddr *>(&a), sizeof(a)) != 0 || listen(p_->listener, 8) != 0)
        throw std::runtime_error("RTSP relay port unavailable");
    p_->accept_thread = std::thread([this] { p_->run(); });
}
void RtspRelay::stop() {
    p_->stop_flag = true;
    if (p_->accept_thread.joinable())
        p_->accept_thread.join();
    if (p_->listener != INVALID_SOCKET) {
        closesocket(p_->listener);
        p_->listener = INVALID_SOCKET;
    }
    for (auto &t : p_->clients)
        if (t.joinable())
            t.join();
    p_->clients.clear();
}
} // namespace avqm
