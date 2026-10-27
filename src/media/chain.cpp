#include "media/chain.hpp"
#include "storage/store.hpp"
#include <iomanip>
#include <sstream>
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libavutil/sha.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}
namespace avqm {
static std::string av_error(int code) {
    char b[256]{};
    av_strerror(code, b, sizeof(b));
    return b;
}
static int interrupted(void *p) {
    return static_cast<Chain *>(p)->stop_requested ? 1 : 0;
}
static int64_t to_us(int64_t pts, AVRational tb) {
    return pts == AV_NOPTS_VALUE ? INT64_MIN : av_rescale_q(pts, tb, AV_TIME_BASE_Q);
}

// Hash VCL NAL payload, ignoring Annex-B/AVCC framing and configuration NALs.
// Never writes identifiers into the media. AAC hashes its unmodified raw payload.
static std::string signature(const AVPacket *p, AVCodecID codec, const AVCodecParameters *par) {
    std::vector<uint8_t> bytes;
    if (codec == AV_CODEC_ID_H264) {
        auto append = [&](const uint8_t *b, size_t n) {
            if (n && ((b[0] & 31) >= 1 && (b[0] & 31) <= 5)) {
                uint32_t len = uint32_t(n);
                for (int k = 3; k >= 0; --k)
                    bytes.push_back(uint8_t(len >> (k * 8)));
                bytes.insert(bytes.end(), b, b + n);
            }
        };
        const auto *d = p->data;
        size_t n = p->size;
        bool annex = n >= 4 && d[0] == 0 && d[1] == 0 && (d[2] == 1 || (d[2] == 0 && d[3] == 1));
        if (annex) {
            size_t start = 0;
            while (start < n) {
                size_t prefix = (start + 3 < n && d[start + 2] == 0) ? 4 : 3;
                size_t next = start + prefix;
                while (next + 3 < n && !(d[next] == 0 && d[next + 1] == 0 &&
                                         (d[next + 2] == 1 || (d[next + 2] == 0 && d[next + 3] == 1))))
                    ++next;
                if (next + 3 >= n)
                    next = n;
                size_t end = next;
                while (end > start + prefix && d[end - 1] == 0)
                    --end;
                append(d + start + prefix, end - start - prefix);
                start = next;
            }
        } else {
            int ls = 4;
            if (par->extradata_size > 4 && par->extradata[0] == 1)
                ls = (par->extradata[4] & 3) + 1;
            size_t off = 0;
            while (off + ls <= n) {
                uint32_t len = 0;
                for (int k = 0; k < ls; ++k)
                    len = (len << 8) | d[off++];
                if (!len || off + len > n)
                    return {};
                append(d + off, len);
                off += len;
            }
        }
    } else if (codec == AV_CODEC_ID_AAC) {
        size_t off = 0;
        if (p->size > 7 && p->data[0] == 0xff && (p->data[1] & 0xf0) == 0xf0)
            off = (p->data[1] & 1) ? 7 : 9;
        if (off < size_t(p->size))
            bytes.assign(p->data + off, p->data + p->size);
    } else
        return {};
    if (bytes.empty())
        return {};
    auto *sha = av_sha_alloc();
    if (!sha)
        return {};
    uint8_t digest[32];
    av_sha_init(sha, 256);
    av_sha_update(sha, bytes.data(), bytes.size());
    av_sha_final(sha, digest);
    av_free(sha);
    std::ostringstream out;
    out << std::hex << std::setfill('0');
    for (auto b : digest)
        out << std::setw(2) << int(b);
    return out.str();
}
Chain::Chain(Application &a, ChainConfig c) : app(a), cfg(std::move(c)) {}
void Chain::injection_truth(const Json &value) {
    std::lock_guard lock(mutex);
    if (truth_.is_open())
        truth_ << value.dump() << '\n';
}
void Chain::rtp_frame_observed(const std::vector<uint8_t> &access_unit, size_t before, size_t after,
                               uint32_t timestamp) {
    AVPacket packet{};
    packet.data = const_cast<uint8_t *>(access_unit.data());
    packet.size = int(access_unit.size());
    AVCodecParameters parameters{};
    auto hash = signature(&packet, AV_CODEC_ID_H264, &parameters);
    std::lock_guard lock(mutex);
    auto it = signatures.find("v" + hash);
    if (it == signatures.end())
        return;
    std::vector<std::shared_ptr<SourceUnit>> candidates;
    for (auto &w : it->second)
        if (auto u = w.lock(); u && now_us() - u->source_us < 15000000)
            candidates.push_back(u);
    if (candidates.size() != 1)
        return;
    auto u = candidates.front();
    observe({{"stage", "rtp_boundary_observation"},
             {"id", u->id},
             {"rtp_timestamp", timestamp},
             {"packets_before", before},
             {"packets_after", after},
             {"mono_us", now_us()}});
    if (after < before) {
        u->frame.damaged = true;
        if (u->frame.outcome == Outcome::pending)
            u->frame.outcome = Outcome::transport_lost;
    }
}
Chain::~Chain() {
    stop();
}
void Chain::observe(const Json &j) {
    if (observations_.is_open())
        observations_ << j.dump() << '\n';
}
void Chain::start() {
    if (!stop_requested)
        return;
    stop();
    {
        std::lock_guard lock(mutex);
        stop_requested = false;
        publisher_ready = false;
        state = "starting";
        error.clear();
        session = cfg.id + "-" + std::to_string(utc_ms());
        started_us = now_us();
        source.clear();
        signatures.clear();
        videos.clear();
        audio.clear();
        sync_samples.clear();
        latency_ms.reset();
        av_offset_ms.reset();
        audio_clock_media.reset();
        audio_clock_observed_us = 0;
        last_audio_logged_us = 0;
        latency_samples.clear();
        presentation_times.clear();
        received_video_units = 0;
        rtp_packets_received = rtp_packets_lost = rtp_packets_duplicates = 0;
        session_max_consecutive = 0;
        video_pts_offset_.reset();
        audio_pts_offset_.reset();
        freeze.reset();
        sent = matched_video = decoded = presented = ambiguous = received_bytes = source_sequence = 0;
        playback_anchor_us = 0;
        first_present_source_us = 0;
        ++playback_generation;
        first_rx_media = 0;
        last_present_us = 0;
        output_validity = audio_validity = "warming_up";
        for (auto &alarm : alarms)
            alarm.reset();
        std::filesystem::create_directories(app.root / "data/observations");
        observations_.open(app.root / "data/observations" / (session + ".jsonl"));
        truth_.open(app.root / "data/observations" / (session + "-truth.jsonl"));
    }
    if (cfg.push_enabled)
        publisher_ = std::thread(&Chain::publish, this);
    else
        publisher_ready = true;
    receiver_ = std::thread(&Chain::receive, this);
    app.event(cfg.id, "started", {{"session", session}});
}
void Chain::stop() {
    stop_requested = true;
    if (publisher_.joinable())
        publisher_.join();
    if (receiver_.joinable())
        receiver_.join();
    std::lock_guard lock(mutex);
    publisher_ready = false;
    state = "stopped";
    videos.clear();
    audio.clear();
    if (observations_.is_open())
        observations_.close();
    if (truth_.is_open())
        truth_.close();
}
void Chain::set_fault(const Json &j) {
    std::lock_guard lock(mutex);
    fault.packet_loss_pct = j.value("packet_loss_pct", 0.0);
    fault.frame_loss_pct = j.value("frame_loss_pct", 0.0);
    fault.presentation_drop_pct = j.value("presentation_drop_pct", 0.0);
    fault.delay_ms = j.value("delay_ms", 0);
    fault.audio_delay_ms = j.value("audio_delay_ms", 0);
    fault.video_delay_ms = j.value("video_delay_ms", 0);
    fault.drift_ms_per_s = j.value("drift_ms_per_s", 0.0);
    fault.seed = j.value("seed", 1u);
    fault.burst_every = j.value("burst_every", 0);
    fault.burst_length = j.value("burst_length", 0);
    fault.started_us = now_us();
}

void Chain::publish() {
    while (!stop_requested) {
        AVFormatContext *input = nullptr;
        AVFormatContext *output = nullptr;
        AVPacket *p = av_packet_alloc();
        bool wrote_header = false;
        try {
            int rc = avformat_open_input(&input, (app.root / cfg.source).string().c_str(), nullptr, nullptr);
            if (rc < 0)
                throw std::runtime_error("source: " + av_error(rc));
            if (avformat_find_stream_info(input, nullptr) < 0)
                throw std::runtime_error("source stream info failed");
            const char *fmt = cfg.publish_url.starts_with("rtmp") ? "flv" : "rtsp";
            avformat_alloc_output_context2(&output, nullptr, fmt, cfg.publish_url.c_str());
            if (!output)
                throw std::runtime_error("output allocation failed");
            output->interrupt_callback = {interrupted, this};
            std::vector<int> stream_map(input->nb_streams, -1);
            for (unsigned i = 0; i < input->nb_streams; ++i) {
                auto *s = input->streams[i];
                if (s->codecpar->codec_type != AVMEDIA_TYPE_VIDEO &&
                    s->codecpar->codec_type != AVMEDIA_TYPE_AUDIO)
                    continue;
                if (s->codecpar->codec_type == AVMEDIA_TYPE_AUDIO && !cfg.has_audio)
                    continue;
                {
                    std::lock_guard lock(mutex);
                    if (s->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
                        width = s->codecpar->width;
                        height = s->codecpar->height;
                        double source_fps = av_q2d(s->avg_frame_rate);
                        if (source_fps > 0)
                            fps = source_fps;
                    } else
                        sample_rate = s->codecpar->sample_rate;
                }
                auto *os = avformat_new_stream(output, nullptr);
                avcodec_parameters_copy(os->codecpar, s->codecpar);
                os->codecpar->codec_tag = 0;
                os->time_base = s->time_base;
                stream_map[i] = os->index;
            }
            AVDictionary *options = nullptr;
            av_dict_set(&options, "rtsp_transport", cfg.transport.c_str(), 0);
            av_dict_set(&options, "rw_timeout", "3000000", 0);
            if (!(output->oformat->flags & AVFMT_NOFILE)) {
                rc = avio_open2(&output->pb, cfg.publish_url.c_str(), AVIO_FLAG_WRITE,
                                &output->interrupt_callback, &options);
                if (rc < 0) {
                    av_dict_free(&options);
                    throw std::runtime_error("publish open: " + av_error(rc));
                }
            }
            rc = avformat_write_header(output, &options);
            av_dict_free(&options);
            if (rc < 0)
                throw std::runtime_error("publish header: " + av_error(rc));
            wrote_header = true;
            publisher_ready = true;
            int64_t first_dts = INT64_MIN, origin = now_us();
            while (!stop_requested && (rc = av_read_frame(input, p)) >= 0) {
                int idx = p->stream_index;
                if (stream_map[idx] < 0) {
                    av_packet_unref(p);
                    continue;
                }
                auto *s = input->streams[idx];
                auto media = to_us(p->pts, s->time_base);
                auto decode_time = to_us(p->dts == AV_NOPTS_VALUE ? p->pts : p->dts, s->time_base);
                if (decode_time == INT64_MIN || media == INT64_MIN) {
                    av_packet_unref(p);
                    continue;
                }
                if (first_dts == INT64_MIN)
                    first_dts = decode_time;
                wait_until(origin + decode_time - first_dts, stop_requested);
                if (stop_requested)
                    break;
                const bool video = s->codecpar->codec_type == AVMEDIA_TYPE_VIDEO;
                auto sig = signature(p, s->codecpar->codec_id, s->codecpar);
                auto unit = std::make_shared<SourceUnit>();
                unit->video = video;
                unit->signature = sig;
                unit->source_us = now_us();
                unit->media_us = media;
                unit->duration_us = to_us(p->duration, s->time_base);
                {
                    std::lock_guard lock(mutex);
                    unit->id = ++source_sequence;
                    unit->frame.id = unit->id;
                    unit->frame.source_us = unit->source_us;
                    unit->frame.media_us = media;
                    source.push_back(unit);
                    if (!sig.empty())
                        signatures[(video ? "v" : "a") + sig].push_back(unit);
                    if (video)
                        ++sent;
                    observe({{"stage", "source_release"},
                             {"id", unit->id},
                             {"video", video},
                             {"media_us", media},
                             {"mono_us", unit->source_us},
                             {"sha256", sig}});
                    while (!source.empty() && unit->source_us - source.front()->source_us > 90000000)
                        source.pop_front();
                    if (source_sequence % 1000 == 0) {
                        for (auto it = signatures.begin(); it != signatures.end();) {
                            auto &v = it->second;
                            std::erase_if(v, [](auto &w) { return w.expired(); });
                            if (v.empty())
                                it = signatures.erase(it);
                            else
                                ++it;
                        }
                    }
                }
                p->stream_index = stream_map[idx];
                av_packet_rescale_ts(p, s->time_base, output->streams[p->stream_index]->time_base);
                p->pos = -1;
                rc = av_interleaved_write_frame(output, p);
                av_packet_unref(p);
                if (rc < 0) {
                    std::lock_guard lock(mutex);
                    unit->frame.outcome = Outcome::send_failed;
                    throw std::runtime_error("publish write: " + av_error(rc));
                }
            }
            if (!stop_requested) { // Loop the original file as a fresh epoch; reset matching/playout to avoid
                                   // timestamp aliasing.
                std::lock_guard lock(mutex);
                source.clear();
                signatures.clear();
                video_pts_offset_.reset();
                audio_pts_offset_.reset();
                playback_anchor_us = 0;
                first_present_source_us = 0;
                ++playback_generation;
                state = "warming_up";
                freeze.reset();
                last_present_us = 0;
                audio_clock_observed_us = 0;
                av_offset_ms.reset();
                latency_ms.reset();
                sync_samples.clear();
                videos.clear();
                audio.clear();
                observe({{"stage", "source_loop"}, {"mono_us", now_us()}});
            }
        } catch (const std::exception &e) {
            std::lock_guard lock(mutex);
            error = e.what();
            state = "reconnecting";
        }
        publisher_ready = false;
        if (output) {
            if (wrote_header)
                av_write_trailer(output);
            if (!(output->oformat->flags & AVFMT_NOFILE) && output->pb)
                avio_closep(&output->pb);
            avformat_free_context(output);
        }
        if (input)
            avformat_close_input(&input);
        av_packet_free(&p);
        if (!stop_requested)
            wait_until(now_us() + 1000000, stop_requested);
    }
}
std::shared_ptr<SourceUnit> Chain::match(const std::string &sig, bool video, int64_t pts) {
    if (sig.empty())
        return {};
    auto it = signatures.find((video ? "v" : "a") + sig);
    if (it == signatures.end())
        return {};
    std::vector<std::shared_ptr<SourceUnit>> candidates;
    auto &offset = video ? video_pts_offset_ : audio_pts_offset_;
    for (auto &weak : it->second)
        if (auto u = weak.lock(); u && !u->matched && now_us() - u->source_us < 15000000) {
            candidates.push_back(u);
        }
    if (candidates.size() > 1 && pts != INT64_MIN && offset)
        std::erase_if(candidates, [&](auto &u) { return std::abs(u->media_us - (pts + *offset)) > 2000; });
    if (candidates.size() != 1) {
        if (candidates.size() > 1)
            ++ambiguous;
        return {};
    }
    auto u = candidates.front();
    u->matched = true;
    if (pts != INT64_MIN)
        offset = u->media_us - pts;
    u->frame.receive_us = now_us();
    if (video)
        ++matched_video;
    observe({{"stage", "received_matched"},
             {"id", u->id},
             {"mono_us", now_us()},
             {"rx_media_us", pts},
             {"source_media_us", u->media_us}});
    return u;
}

void Chain::receive() {
    while (!stop_requested) {
        while (!stop_requested && !publisher_ready)
            wait_until(now_us() + 100000, stop_requested);
        if (stop_requested)
            break;
        AVFormatContext *in = avformat_alloc_context();
        in->interrupt_callback = {interrupted, this};
        AVPacket *p = av_packet_alloc();
        AVFrame *frame = av_frame_alloc();
        std::vector<AVCodecContext *> codecs;
        SwsContext *sws = nullptr;
        SwrContext *swr = nullptr;
        try {
            AVDictionary *opts = nullptr;
            if (cfg.protocol == "rtsp") {
                av_dict_set(&opts, "rtsp_transport", cfg.transport.c_str(), 0);
                av_dict_set(&opts, "timeout", "3000000", 0);
                av_dict_set(&opts, "buffer_size", "4194304", 0);
            }
            av_dict_set(&opts, "rw_timeout", "3000000", 0);
            av_dict_set(&opts, "analyzeduration", "100000", 0);
            av_dict_set(&opts, "probesize", "131072", 0);
            int rc = avformat_open_input(&in, cfg.pull_url.c_str(), nullptr, &opts);
            av_dict_free(&opts);
            if (rc < 0)
                throw std::runtime_error("pull open: " + av_error(rc));
            if (avformat_find_stream_info(in, nullptr) < 0)
                throw std::runtime_error("pull stream info failed");
            codecs.resize(in->nb_streams, nullptr);
            for (unsigned i = 0; i < in->nb_streams; ++i) {
                auto *par = in->streams[i]->codecpar;
                if (par->codec_type != AVMEDIA_TYPE_VIDEO && par->codec_type != AVMEDIA_TYPE_AUDIO)
                    continue;
                if (par->codec_type == AVMEDIA_TYPE_AUDIO && !cfg.has_audio)
                    continue;
                auto *codec = avcodec_find_decoder(par->codec_id);
                if (!codec)
                    continue;
                auto *c = avcodec_alloc_context3(codec);
                avcodec_parameters_to_context(c, par);
                c->pkt_timebase = in->streams[i]->time_base;
                c->thread_count = 2;
                c->flags |= AV_CODEC_FLAG_COPY_OPAQUE;
                if (avcodec_open2(c, codec, nullptr) < 0) {
                    avcodec_free_context(&c);
                    continue;
                }
                codecs[i] = c;
                std::lock_guard lock(mutex);
                if (par->codec_type == AVMEDIA_TYPE_VIDEO) {
                    width = par->width;
                    height = par->height;
                    if (av_q2d(in->streams[i]->avg_frame_rate) > 0)
                        fps = av_q2d(in->streams[i]->avg_frame_rate);
                } else
                    sample_rate = par->sample_rate;
                state = "warming_up";
                error.clear();
            }
            std::vector<std::map<int64_t, std::shared_ptr<SourceUnit>>> pending(in->nb_streams);
            std::mt19937 rng(1);
            uint32_t rng_seed = 1;
            uint64_t counter = 0;
            while (!stop_requested && (rc = av_read_frame(in, p)) >= 0) {
                auto idx = p->stream_index;
                auto *s = in->streams[idx];
                auto *codec = codecs[idx];
                if (!codec) {
                    av_packet_unref(p);
                    continue;
                }
                bool video = s->codecpar->codec_type == AVMEDIA_TYPE_VIDEO;
                auto pts = to_us(p->pts, s->time_base);
                auto sig = signature(p, s->codecpar->codec_id, s->codecpar);
                std::shared_ptr<SourceUnit> unit;
                FaultConfig f;
                {
                    std::lock_guard lock(mutex);
                    received_bytes += p->size;
                    if (video && !sig.empty())
                        ++received_video_units;
                    unit = match(sig, video, pts);
                    f = fault;
                    if (unit)
                        pending[idx][p->pts] = unit;
                }
                // Complete-access-unit reception loss injection. It is NOT reported as network packet loss.
                if (rng_seed != f.seed) {
                    rng_seed = f.seed;
                    rng.seed(rng_seed);
                }
                bool dropped = video && f.frame_loss_pct > 0 &&
                               std::uniform_real_distribution<>(0, 100)(rng) < f.frame_loss_pct;
                if (dropped) {
                    std::lock_guard lock(mutex);
                    if (unit) {
                        unit->frame.outcome = Outcome::receiver_discarded;
                        observe({{"stage", "compressed_unit_discarded"},
                                 {"id", unit->id},
                                 {"boundary", "after_receive_before_decode"},
                                 {"mono_us", now_us()}});
                    }
                    truth_ << Json({{"action", "compressed_unit_discarded"},
                                    {"id", unit ? unit->id : 0},
                                    {"mono_us", now_us()}})
                                  .dump()
                           << '\n';
                    av_packet_unref(p);
                    continue;
                }
                if (unit) {
                    av_buffer_unref(&p->opaque_ref);
                    p->opaque_ref = av_buffer_alloc(sizeof(uint64_t));
                    if (p->opaque_ref)
                        std::memcpy(p->opaque_ref->data, &unit->id, sizeof(uint64_t));
                }
                rc = avcodec_send_packet(codec, p);
                av_packet_unref(p);
                if (rc < 0) {
                    if (unit) {
                        std::lock_guard lock(mutex);
                        unit->frame.outcome = Outcome::decode_failed;
                    }
                    continue;
                }
                while (!stop_requested && avcodec_receive_frame(codec, frame) >= 0) {
                    int64_t fpts = frame->best_effort_timestamp;
                    auto found = pending[idx].find(fpts);
                    auto u = found == pending[idx].end() ? std::shared_ptr<SourceUnit>{} : found->second;
                    if (found != pending[idx].end())
                        pending[idx].erase(found);
                    if (frame->opaque_ref && frame->opaque_ref->size >= sizeof(uint64_t)) {
                        uint64_t source_id{};
                        std::memcpy(&source_id, frame->opaque_ref->data, sizeof(source_id));
                        std::lock_guard lock(mutex);
                        for (auto &entry : source)
                            if (entry->id == source_id) {
                                u = entry;
                                break;
                            }
                    }
                    int64_t media = 0, due = 0;
                    {
                        std::lock_guard lock(mutex);
                        auto offset = video ? video_pts_offset_ : audio_pts_offset_;
                        if (u)
                            media = u->media_us;
                        else if (offset && fpts != AV_NOPTS_VALUE && std::abs(*offset) < 86400000000LL)
                            media = to_us(fpts, s->time_base) + *offset;
                        else if (!cfg.push_enabled && fpts != AV_NOPTS_VALUE)
                            media = to_us(fpts, s->time_base);
                        else {
                            av_frame_unref(frame);
                            continue;
                        }
                        if (!playback_anchor_us) {
                            playback_anchor_us = now_us() + 200000;
                            first_rx_media = media;
                        }
                        due = playback_anchor_us + media - first_rx_media;
                        if (counter < 2)
                            observe({{"stage", "playback_mapping_debug"},
                                     {"video", video},
                                     {"frame_pts", fpts},
                                     {"media_us", media},
                                     {"first_media_us", first_rx_media},
                                     {"anchor_us", playback_anchor_us},
                                     {"due_us", due},
                                     {"now_us", now_us()},
                                     {"matched", bool(u)}});
                        if (video) {
                            width = frame->width;
                            height = frame->height;
                            ++decoded;
                            if (u)
                                u->frame.decode_us = now_us();
                        }
                    }
                    if (video) {
                        ++counter;
                        bool present_drop =
                            f.presentation_drop_pct > 0 &&
                            std::uniform_real_distribution<>(0, 100)(rng) < f.presentation_drop_pct;
                        if (f.burst_every > 0 && counter % f.burst_every < uint64_t(f.burst_length))
                            present_drop = true;
                        if (present_drop && u) {
                            did_drop(u->id, "presentation_dropped");
                            std::lock_guard lock(mutex);
                            truth_ << Json({{"action", "presentation_drop"},
                                            {"id", u->id},
                                            {"mono_us", now_us()}})
                                          .dump()
                                   << '\n';
                            av_frame_unref(frame);
                            continue;
                        }
                        VideoImage image;
                        image.width = frame->width;
                        image.height = frame->height;
                        image.frame_id = u ? u->id : 0;
                        image.source_us = u ? u->source_us : 0;
                        image.media_us = media;
                        image.due_us = due + int64_t(f.video_delay_ms + f.delay_ms) * 1000;
                        image.bgra.resize(size_t(image.width) * image.height * 4);
                        sws = sws_getCachedContext(
                            sws, frame->width, frame->height, AVPixelFormat(frame->format), frame->width,
                            frame->height, AV_PIX_FMT_BGRA, SWS_FAST_BILINEAR, nullptr, nullptr, nullptr);
                        uint8_t *dest[] = {image.bgra.data()};
                        int lines[] = {frame->width * 4};
                        sws_scale(sws, frame->data, frame->linesize, 0, frame->height, dest, lines);
                        while (!stop_requested) {
                            {
                                std::lock_guard lock(mutex);
                                if (videos.size() < 100) {
                                    videos.push_back(std::move(image));
                                    break;
                                }
                            }
                            wait_until(now_us() + 3000, stop_requested);
                        }
                    } else {
                        if (!swr) {
                            AVChannelLayout stereo = AV_CHANNEL_LAYOUT_STEREO;
                            swr_alloc_set_opts2(&swr, &stereo, AV_SAMPLE_FMT_FLT, 48000, &frame->ch_layout,
                                                AVSampleFormat(frame->format), frame->sample_rate, 0,
                                                nullptr);
                            if (!swr || swr_init(swr) < 0)
                                throw std::runtime_error("audio resampler initialization failed");
                        }
                        int capacity = swr_get_out_samples(swr, frame->nb_samples);
                        AudioChunk chunk;
                        chunk.samples.resize(size_t(capacity) * 2);
                        uint8_t *out[] = {reinterpret_cast<uint8_t *>(chunk.samples.data())};
                        int n = swr_convert(swr, out, capacity,
                                            const_cast<const uint8_t **>(frame->extended_data),
                                            frame->nb_samples);
                        if (n > 0) {
                            chunk.samples.resize(size_t(n) * 2);
                            chunk.media_us = media;
                            chunk.mapped = bool(u) || audio_pts_offset_.has_value() || !cfg.push_enabled;
                            chunk.due_us =
                                due + int64_t(f.audio_delay_ms) * 1000 +
                                int64_t(f.drift_ms_per_s * (now_us() - f.started_us) / 1000000.0) * 1000 +
                                int64_t(f.delay_ms) * 1000;
                            std::lock_guard lock(mutex);
                            if (audio.size() > 240) {
                                audio.pop_front();
                                audio_validity = "audio_queue_overflow";
                            }
                            audio.push_back(std::move(chunk));
                        }
                    }
                    av_frame_unref(frame);
                }
                for (auto &map : pending)
                    while (map.size() > 300)
                        map.erase(map.begin());
            }
            if (!stop_requested)
                throw std::runtime_error("pull ended: " + av_error(rc));
        } catch (const std::exception &e) {
            std::lock_guard lock(mutex);
            error = e.what();
            state = "reconnecting";
            output_validity = "stale";
            audio_validity = "stale";
        }
        for (auto *&c : codecs)
            avcodec_free_context(&c);
        sws_freeContext(sws);
        swr_free(&swr);
        av_frame_free(&frame);
        av_packet_free(&p);
        if (in)
            avformat_close_input(&in);
        if (!stop_requested)
            wait_until(now_us() + 1000000, stop_requested);
    }
}
void Chain::did_drop(uint64_t id, const std::string &reason) {
    std::lock_guard lock(mutex);
    for (auto &u : source)
        if (u->id == id) {
            u->frame.outcome = Outcome::presentation_dropped;
            break;
        }
    observe({{"stage", reason}, {"id", id}, {"mono_us", now_us()}});
}
void Chain::did_present(uint64_t id, int64_t media, int64_t source_time, int64_t observed) {
    std::lock_guard lock(mutex);
    ++presented;
    last_video_media = media;
    last_present_us = observed;
    if (audio_clock_media && observed >= audio_clock_observed_us &&
        observed - audio_clock_observed_us < 15000) {
        av_offset_ms = sync_offset_ms(*audio_clock_media + observed - audio_clock_observed_us, media);
        sync_samples.push_back({(observed - started_us) / 1000000.0, *av_offset_ms});
        while (!sync_samples.empty() && sync_samples.back().seconds - sync_samples.front().seconds > 60)
            sync_samples.erase(sync_samples.begin());
    }
    presentation_times.push_back(observed);
    while (presentation_times.size() > 300 ||
           (!presentation_times.empty() && observed - presentation_times.front() > 1000000))
        presentation_times.pop_front();
    freeze.presented(observed);
    if (source_time) {
        if (!first_present_source_us)
            first_present_source_us = source_time;
        latency_ms = measured_latency_ms(source_time, observed);
        if (latency_ms)
            latency_samples.emplace_back(observed, *latency_ms);
        while (!latency_samples.empty() && observed - latency_samples.front().first > 1000000)
            latency_samples.pop_front();
    } else
        latency_ms.reset();
    for (auto &u : source)
        if (u->id == id) {
            u->frame.outcome = Outcome::presented;
            u->frame.present_us = observed;
            break;
        }
    output_validity = "software_render_complete";
    state = "running";
    observe({{"stage", "software_present"},
             {"id", id},
             {"mono_us", observed},
             {"media_us", media},
             {"source_us", source_time},
             {"backend", "Direct2D_WIC_pixels_ready"}});
}
void Chain::audio_progress(int64_t media, int64_t observed, uint64_t device_position,
                           uint64_t device_frequency) {
    std::lock_guard lock(mutex);
    audio_clock_media = media;
    audio_clock_observed_us = observed;
    audio_validity = "device_clock";
    if (observed - last_audio_logged_us >= 20000) {
        observe({{"stage", "audio_device_position"},
                 {"media_us", media},
                 {"mono_us", observed},
                 {"device_position", device_position},
                 {"device_frequency", device_frequency}});
        last_audio_logged_us = observed;
    }
}
Json Chain::snapshot() {
    auto settings = app.settings_copy();
    std::lock_guard lock(mutex);
    auto now = now_us();
    std::vector<FrameRecord> recent;
    for (auto &u : source)
        if (u->video && first_present_source_us && u->source_us >= first_present_source_us &&
            now - u->source_us <= (settings.resolution_timeout_ms + 10000) * 1000LL) {
            auto f = u->frame;
            if (f.outcome == Outcome::pending &&
                now - u->source_us > settings.resolution_timeout_ms * 1000LL) {
                f.outcome = Outcome::unknown_missing;
                u->frame.outcome = f.outcome;
            }
            if (now - u->source_us >= settings.resolution_timeout_ms * 1000LL)
                recent.push_back(f);
        }
    std::sort(recent.begin(), recent.end(), [](auto &a, auto &b) { return a.media_us < b.media_us; });
    auto loss = summarize_loss(recent);
    std::optional<double> latency_mean, latency_max, sync_median;
    if (now - last_present_us < 500000 && !latency_samples.empty()) {
        double sum = 0, mx = 0;
        for (auto [time, value] : latency_samples) {
            sum += value;
            mx = std::max(mx, value);
        }
        latency_mean = sum / latency_samples.size();
        latency_max = mx;
    }
    if (now - audio_clock_observed_us < 100000 && now - last_present_us < 500000) {
        std::vector<double> offsets;
        double t = (now - started_us) / 1000000.0;
        for (auto it = sync_samples.rbegin(); it != sync_samples.rend() && t - it->seconds <= 1; ++it)
            offsets.push_back(it->offset_ms);
        if (!offsets.empty()) {
            std::sort(offsets.begin(), offsets.end());
            sync_median = offsets[offsets.size() / 2];
        }
    }
    session_max_consecutive = std::max<uint64_t>(session_max_consecutive, loss.consecutive_max);
    Json j = {
        {"id", cfg.id},
        {"alias", cfg.alias},
        {"protocol", cfg.protocol},
        {"transport", cfg.transport},
        {"state", state},
        {"error", error},
        {"session", session},
        {"time_ms", utc_ms()},
        {"width", width},
        {"height", height},
        {"fps", fps},
        {"audio_sample_rate", sample_rate},
        {"bitrate_kbps",
         started_us ? (received_bytes * 8.0 / 1000) / std::max(0.1, (now - started_us) / 1000000.0) : 0},
        {"uptime_s", started_us ? (now - started_us) / 1000000.0 : 0},
        {"sent_frames", sent},
        {"matched_frames", matched_video},
        {"decoded_frames", decoded},
        {"presented_frames", presented},
        {"output_fps", presentation_times.size() > 1 && now - presentation_times.back() < 500000
                           ? (presentation_times.size() - 1) * 1000000.0 /
                                 (presentation_times.back() - presentation_times.front())
                           : 0.0},
        {"video_queue_size", videos.size()},
        {"video_due_lag_ms", videos.empty() ? 0.0 : (now - videos.front().due_us) / 1000.0},
        {"first_rx_media_us", first_rx_media},
        {"ambiguous", ambiguous},
        {"rtp_packets_received", rtp_packets_received},
        {"rtp_packets_lost", rtp_packets_lost},
        {"rtp_packets_duplicates", rtp_packets_duplicates},
        {"rtp_loss_pct",
         (cfg.protocol == "rtsp" && cfg.transport == "udp" && (rtp_packets_received + rtp_packets_lost))
             ? Json(100.0 * rtp_packets_lost / (rtp_packets_received + rtp_packets_lost))
             : Json(nullptr)},
        {"association_coverage_pct", received_video_units ? 100.0 * matched_video / received_video_units : 0},
        {"source_delivery_match_pct", sent ? 100.0 * matched_video / sent : 0},
        {"latency_ms", latency_mean ? Json(*latency_mean) : Json(nullptr)},
        {"latency_max_ms", latency_max ? Json(*latency_max) : Json(nullptr)},
        {"latency_sample_count", latency_samples.size()},
        {"av_offset_ms", sync_median ? Json(*sync_median) : Json(nullptr)},
        {"loss_pct", loss.percent ? Json(*loss.percent) : Json(nullptr)},
        {"loss_expected", loss.expected},
        {"loss_missing", loss.missing},
        {"loss_pending", loss.pending},
        {"loss_reasons", loss.reasons},
        {"consecutive_max", session_max_consecutive},
        {"freeze_ms", freeze.stalled_ms(now)},
        {"frozen", freeze.frozen(now)},
        {"freeze_count", freeze.completed_count},
        {"output_validity", now - last_present_us < 500000 ? output_validity : "stale"},
        {"audio_validity", !cfg.has_audio                           ? "not_applicable"
                           : now - audio_clock_observed_us < 100000 ? audio_validity
                                                                    : "stale"},
        {"source_reference", cfg.push_enabled ? "managed_source_release" : "unavailable_pull_only"},
        {"fault",
         {{"packet_loss_pct", fault.packet_loss_pct},
          {"frame_loss_pct", fault.frame_loss_pct},
          {"presentation_drop_pct", fault.presentation_drop_pct},
          {"delay_ms", fault.delay_ms},
          {"audio_delay_ms", fault.audio_delay_ms},
          {"video_delay_ms", fault.video_delay_ms},
          {"drift_ms_per_s", fault.drift_ms_per_s}}}};
    auto fit = fit_drift(sync_samples);
    j["drift_ms_per_s"] =
        (fit && sync_samples.size() > 40 && sync_samples.back().seconds - sync_samples.front().seconds > 48)
            ? Json(fit->slope_ms_per_s)
            : Json(nullptr);
    j["levels"] = Json::array();
    for (auto &a : alarms)
        j["levels"].push_back(level_name(a.level()));
    return j;
}
void Chain::tick() {
    auto j = snapshot();
    auto settings = app.settings_copy();
    std::vector<std::optional<Level>> targets(5);
    if (j["latency_ms"].is_number() && j["output_validity"] != "stale") {
        double v = j["latency_ms"];
        targets[0] = v > settings.delay_critical_ms  ? Level::critical
                     : v > settings.delay_warning_ms ? Level::warning
                                                     : Level::normal;
    }
    if (j["av_offset_ms"].is_number()) {
        double v = j["av_offset_ms"];
        targets[1] = (v > settings.av_lead_critical_ms || v < -settings.av_lag_critical_ms) ? Level::critical
                     : (v > settings.av_lead_warning_ms || v < -settings.av_lag_warning_ms) ? Level::warning
                                                                                            : Level::normal;
    }
    if (j["loss_pct"].is_number()) {
        double v = j["loss_pct"];
        targets[2] = v > settings.loss_critical_pct  ? Level::critical
                     : v > settings.loss_warning_pct ? Level::warning
                                                     : Level::normal;
    }
    if (j["state"] == "running")
        targets[3] =
            j["freeze_ms"].get<double>() >= settings.freeze_critical_ms ? Level::critical : Level::normal;
    if (j["drift_ms_per_s"].is_number())
        targets[4] = std::abs(j["drift_ms_per_s"].get<double>()) > settings.drift_warning_ms_per_s
                         ? Level::warning
                         : Level::normal;
    const char *names[] = {"latency", "av_sync", "frame_loss", "freeze", "sync_drift"};
    for (size_t i = 0; i < 5; ++i) {
        std::optional<AlertChange> change;
        {
            std::lock_guard lock(mutex);
            change = alarms[i].update(
                targets[i], now_us(),
                i == 3 ? 0 : int64_t(i == 4 ? settings.drift_hold_ms : settings.hold_ms) * 1000,
                int64_t(settings.recovery_ms) * 1000);
        }
        if (change)
            app.event(cfg.id, "alert",
                      {{"metric", names[i]},
                       {"transition", change->transition},
                       {"level", level_name(change->after)},
                       {"snapshot", j}});
    }
    app.store->sample(j);
}
} // namespace avqm
