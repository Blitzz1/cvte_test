#include "media/chain.hpp"
#include "playback/video_convert.hpp"
#include "storage/observations.hpp"
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
    app.journal->append(session, value.dump(), true);
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
    }
}
Chain::~Chain() {
    stop();
}
void Chain::observe(const Json &j) {
    if (!session.empty())
        app.journal->append(session, j.dump());
}
void Chain::discard_locked(uint64_t id, const std::string &reason, uint64_t generation) {
    if (generation != playback_generation)
        return;
    last_video_drop_us = now_us();
    auto it = source_ids.find(id);
    if (it != source_ids.end())
        if (auto u = it->second.lock())
            if (u->frame.outcome != Outcome::presented)
                u->frame.outcome = Outcome::presentation_dropped;
    observe({{"stage", "presentation_dropped"},
             {"reason", reason},
             {"id", id},
             {"generation", generation},
             {"mono_us", now_us()}});
}
void Chain::reset_playback_locked(const std::string &reason) {
    preview.clear();
    for (auto &image : videos)
        discard_locked(image.frame_id, reason, image.generation);
    videos.clear();
    video_queue_bytes = 0;
    audio.clear();
    ++playback_generation;
    ++sync_segment;
    audio_sample.reset();
    audio_clock_media.reset();
    audio_clock_observed_us = 0;
    last_present_us = 0;
    playback_anchor_us = 0;
    first_present_source_us = 0;
    first_rx_media = 0;
    video_pts_offset_.reset();
    audio_pts_offset_.reset();
    last_video_match_us_ = 0;
    sync_samples.clear();
    sync_step = {};
    latency_samples.clear();
    presentation_times.clear();
    freeze.reset();
    unmapped_outputs.clear();
    last_video_source_valid = false;
    last_video_drop_us = 0;
    latency_ms.reset();
    av_offset_ms.reset();
    output_validity = audio_validity = "warming_up";
    observe({{"stage", "playback_epoch"},
             {"generation", playback_generation},
             {"reason", reason},
             {"mono_us", now_us()}});
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
        preview.clear();
        started_us = now_us();
        source.clear();
        source_ids.clear();
        video_media_index.clear();
        signatures.clear();
        videos.clear();
        video_queue_bytes = video_queue_peak_bytes = 0;
        video_budget_drops = video_late_drops = timestamp_recovered = 0;
        output_lock_skips = 0;
        conversion_ms = render_work_ms = 0;
        audio_sample.reset();
        ++sync_segment;
        audio.clear();
        sync_samples.clear();
        sync_step = {};
        unmapped_outputs.clear();
        last_video_source_valid = false;
        last_video_drop_us = 0;
        latency_ms.reset();
        av_offset_ms.reset();
        audio_clock_media.reset();
        audio_clock_observed_us = 0;
        last_audio_logged_us = 0;
        latency_samples.clear();
        presentation_times.clear();
        received_video_units = 0;
        receiver_connection = 0;
        byte_samples.clear();
        rtp_packets_received = rtp_packets_lost = rtp_packets_duplicates = 0;
        session_max_consecutive = 0;
        video_pts_offset_.reset();
        audio_pts_offset_.reset();
        freeze.reset();
        completed_freezes.clear();
        sent = matched_video = decoded = presented = ambiguous = received_bytes = source_sequence = 0;
        playback_anchor_us = 0;
        first_present_source_us = 0;
        ++playback_generation;
        first_rx_media = 0;
        last_present_us = 0;
        output_validity = audio_validity = "warming_up";
        for (auto &alarm : alarms)
            alarm.reset();
        observe({{"stage", "session_started"}, {"generation", playback_generation}, {"mono_us", now_us()}});
    }
    if (cfg.push_enabled)
        publisher_ = std::thread(&Chain::publish, this);
    else
        publisher_ready = true;
    receiver_ = std::thread(&Chain::receive, this);
    app.event(cfg.id, "started", {{"session", session}});
}
void Chain::stop() {
    if (stop_requested && !publisher_.joinable() && !receiver_.joinable())
        return;
    stop_requested = true;
    {
        std::lock_guard lock(mutex);
        state = "stopping";
        reset_playback_locked("stopping");
    }
    if (publisher_.joinable())
        publisher_.join();
    if (receiver_.joinable())
        receiver_.join();
    std::lock_guard lock(mutex);
    publisher_ready = false;
    state = "stopped";
    videos.clear();
    video_queue_bytes = 0;
    audio.clear();
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
            int64_t first_dts = INT64_MIN, origin = now_us(), epoch_offset = 0, epoch_end = 0;
            bool epoch_has_packets = false;
            while (!stop_requested) {
                rc = av_read_frame(input, p);
                if (rc < 0) {
                    if (rc == AVERROR_EOF && epoch_has_packets &&
                        av_seek_frame(input, -1, 0, AVSEEK_FLAG_BACKWARD) >= 0) {
                        avformat_flush(input);
                        epoch_offset = epoch_end;
                        epoch_has_packets = false;
                        std::lock_guard lock(mutex);
                        observe({{"stage", "source_loop"},
                                 {"mono_us", now_us()},
                                 {"media_epoch_offset_us", epoch_offset}});
                        continue;
                    }
                    throw std::runtime_error("source read: " + av_error(rc));
                }
                int idx = p->stream_index;
                if (stream_map[idx] < 0) {
                    av_packet_unref(p);
                    continue;
                }
                auto *s = input->streams[idx];
                auto shift = av_rescale_q(epoch_offset, AV_TIME_BASE_Q, s->time_base);
                if (p->pts != AV_NOPTS_VALUE)
                    p->pts += shift;
                if (p->dts != AV_NOPTS_VALUE)
                    p->dts += shift;
                auto media = to_us(p->pts, s->time_base);
                auto decode_time = to_us(p->dts == AV_NOPTS_VALUE ? p->pts : p->dts, s->time_base);
                if (decode_time == INT64_MIN || media == INT64_MIN) {
                    av_packet_unref(p);
                    continue;
                }
                epoch_has_packets = true;
                epoch_end =
                    std::max(epoch_end, media + std::max<int64_t>(1, to_us(p->duration, s->time_base)));
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
                    source_ids[unit->id] = unit;
                    if (video)
                        video_media_index.emplace(media, unit);
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
                    while (!source.empty() && unit->source_us - source.front()->source_us > 90000000) {
                        auto old = source.front();
                        source_ids.erase(old->id);
                        if (old->video) {
                            auto range = video_media_index.equal_range(old->media_us);
                            for (auto it = range.first; it != range.second;) {
                                auto candidate = it->second.lock();
                                if (!candidate || candidate->id == old->id)
                                    it = video_media_index.erase(it);
                                else
                                    ++it;
                            }
                        }
                        source.pop_front();
                    }
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
        } catch (const std::exception &e) {
            std::lock_guard lock(mutex);
            error = e.what();
            state = "reconnecting";
            reset_playback_locked("publisher_reconnect");
            source.clear();
            source_ids.clear();
            video_media_index.clear();
            signatures.clear();
            video_pts_offset_.reset();
            audio_pts_offset_.reset();
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
        u->received_media_us = pts;
    if (pts != INT64_MIN)
        offset = u->media_us - pts;
    u->frame.receive_us = now_us();
    if (video) {
        ++matched_video;
        last_video_match_us_ = now_us();
    }
    observe({{"stage", "received_matched"},
             {"id", u->id},
             {"mono_us", now_us()},
             {"rx_media_us", pts},
             {"generation", playback_generation},
             {"video", video},
             {"evidence", "payload_sha256"},
             {"source_media_us", u->media_us}});
    return u;
}
std::shared_ptr<SourceUnit> Chain::match_damaged_pts(int64_t pts) {
    if (pts == INT64_MIN || !video_pts_offset_ || now_us() - last_video_match_us_ > 500000)
        return {};
    auto mapped = pts + *video_pts_offset_;
    std::shared_ptr<SourceUnit> found;
    for (auto it = video_media_index.lower_bound(mapped - 2000);
         it != video_media_index.end() && it->first <= mapped + 2000; ++it) {
        if (auto u = it->second.lock();
            u && !u->matched && u->frame.damaged && now_us() - u->source_us < 15000000) {
            if (found)
                return {};
            found = u;
        }
    }
    if (found) {
        found->matched = true;
        found->received_media_us = pts;
        found->frame.receive_us = now_us();
        ++matched_video;
        ++timestamp_recovered;
        observe({{"stage", "received_matched"},
                 {"id", found->id},
                 {"mono_us", now_us()},
                 {"rx_media_us", pts},
                 {"source_media_us", found->media_us},
                 {"video", true},
                 {"generation", playback_generation},
                 {"evidence", "rtp_boundary_and_validated_pts"}});
    }
    return found;
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
        VideoConverter converter;
        SwrContext *swr = nullptr;
        try {
            AVDictionary *opts = nullptr;
            if (cfg.protocol == "rtsp") {
                av_dict_set(&opts, "rtsp_transport", cfg.transport.c_str(), 0);
                av_dict_set(&opts, "timeout", "3000000", 0);
                av_dict_set(&opts, "buffer_size", "4194304", 0);
            }
            av_dict_set(&opts, "rw_timeout", "3000000", 0);
            if (cfg.protocol == "httpflv")
                av_dict_set(&opts, "seekable", "0", 0);
            av_dict_set(&opts, "analyzeduration", "100000", 0);
            av_dict_set(&opts, "probesize", "131072", 0);
            int rc = avformat_open_input(&in, cfg.pull_url.c_str(), nullptr, &opts);
            av_dict_free(&opts);
            if (rc < 0)
                throw std::runtime_error("pull open: " + av_error(rc));
            if (avformat_find_stream_info(in, nullptr) < 0)
                throw std::runtime_error("pull stream info failed");
            uint64_t generation = 0;
            auto receive_settings = app.settings_copy();
            {
                std::lock_guard lock(mutex);
                ++receiver_connection;
                session_max_consecutive = 0;
                reset_playback_locked("receiver_connected");
                generation = playback_generation;
                observe({{"stage", "receiver_connected"},
                         {"connection", receiver_connection},
                         {"mono_us", now_us()}});
            }
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
                c->thread_count = receive_settings.decoder_threads;
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
            std::vector<std::map<int64_t, std::weak_ptr<SourceUnit>>> pending(in->nb_streams);
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
                    if (generation != playback_generation)
                        throw std::runtime_error("playback epoch changed");
                    received_bytes += p->size;
                    auto receive_time = now_us();
                    byte_samples.emplace_back(receive_time, uint64_t(p->size));
                    while (!byte_samples.empty() && receive_time - byte_samples.front().first > 1000000)
                        byte_samples.pop_front();
                    if (video)
                        ++received_video_units;
                    unit = match(sig, video, pts);
                    if (!unit && video)
                        unit = match_damaged_pts(pts);
                    f = fault;
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
                    app.journal->append(session,
                                        Json({{"action", "compressed_unit_discarded"},
                                              {"id", unit ? unit->id : 0},
                                              {"mono_us", now_us()}})
                                            .dump(),
                                        true);
                    av_packet_unref(p);
                    continue;
                }
                if (unit) {
                    // Decoder timestamp fallback must not retain dropped/unreturned units forever.
                    pending[idx][p->pts] = unit;
                    while (pending[idx].size() > 512)
                        pending[idx].erase(pending[idx].begin());
                    av_buffer_unref(&p->opaque_ref);
                    p->opaque_ref = av_buffer_alloc(sizeof(uint64_t));
                    if (p->opaque_ref)
                        std::memcpy(p->opaque_ref->data, &unit->id, sizeof(uint64_t));
                }
                auto packet_pts = p->pts;
                const auto packet_flags = p->flags;
                const auto packet_size = p->size;
                rc = avcodec_send_packet(codec, p);
                if (video) {
                    std::lock_guard lock(mutex);
                    observe({{"stage", "decode_submission"}, {"id", unit ? unit->id : 0},
                             {"packet_pts", packet_pts}, {"packet_flags", packet_flags},
                             {"packet_size", packet_size}, {"result", rc}, {"mono_us", now_us()},
                             {"generation", generation}});
                }
                av_packet_unref(p);
                if (rc < 0) {
                    pending[idx].erase(packet_pts);
                    if (unit) {
                        std::lock_guard lock(mutex);
                        unit->frame.outcome =
                            unit->frame.damaged ? Outcome::transport_lost : Outcome::decode_failed;
                        observe({{"stage", "decode_failed"},
                                 {"id", unit->id},
                                 {"generation", generation},
                                 {"transport_damaged", unit->frame.damaged},
                                 {"mono_us", now_us()}});
                    }
                    continue;
                }
                while (!stop_requested && avcodec_receive_frame(codec, frame) >= 0) {
                    int64_t fpts = frame->best_effort_timestamp;
                    auto found = pending[idx].find(fpts);
                    auto u =
                        found == pending[idx].end() ? std::shared_ptr<SourceUnit>{} : found->second.lock();
                    if (found != pending[idx].end())
                        pending[idx].erase(found);
                    if (frame->opaque_ref && frame->opaque_ref->size >= sizeof(uint64_t)) {
                        uint64_t source_id{};
                        std::memcpy(&source_id, frame->opaque_ref->data, sizeof(source_id));
                        std::lock_guard lock(mutex);
                        auto entry = source_ids.find(source_id);
                        if (entry != source_ids.end())
                            u = entry->second.lock();
                    }
                    if (video) {
                        std::lock_guard lock(mutex);
                        observe({{"stage", "decoded_output"}, {"id", u ? u->id : 0},
                                 {"frame_pts", frame->pts}, {"best_effort_pts", fpts},
                                 {"mono_us", now_us()}, {"generation", generation}});
                    }
                    int64_t media = 0, due = 0;
                    {
                        std::lock_guard lock(mutex);
                        if (generation != playback_generation)
                            throw std::runtime_error("decoded frame belongs to old playback epoch");
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
                        if (u && !video && fpts != AV_NOPTS_VALUE && u->received_media_us) {
                            auto adjustment = to_us(fpts, s->time_base) - *u->received_media_us;
                            if (std::abs(adjustment) <= 100000)
                                media += adjustment;
                            else
                                u.reset();
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
                            did_drop(u->id, "injected_presentation_drop", generation);
                            std::lock_guard lock(mutex);
                            app.journal->append(
                                session,
                                Json({{"action", "presentation_drop"}, {"id", u->id}, {"mono_us", now_us()}})
                                    .dump(),
                                true);
                            av_frame_unref(frame);
                            continue;
                        }
                        VideoImage image;
                        auto scale = std::min(double(receive_settings.output_width) / frame->width,
                                              double(receive_settings.output_height) / frame->height);
                        image.width = std::max(1, int(frame->width * scale));
                        image.height = std::max(1, int(frame->height * scale));
                        image.generation = generation;
                        image.frame_id = u ? u->id : 0;
                        image.source_us = u ? u->source_us : 0;
                        image.media_us = media;
                        image.due_us = due + int64_t(f.video_delay_ms + f.delay_ms) * 1000;
                        image.stride = (image.width * 4 + 31) & ~31;
                        auto bytes = size_t(image.stride) * image.height;
                        bool room = false;
                        {
                            std::lock_guard lock(mutex);
                            if (generation == playback_generation && !stop_requested) {
                                if (image.due_us < now_us() - 100000) {
                                    ++video_late_drops;
                                    discard_locked(image.frame_id, "decode_arrived_late", generation);
                                } else if (videos.size() < size_t(receive_settings.video_queue_frames) &&
                                           video_queue_bytes + bytes <=
                                               size_t(receive_settings.video_queue_mib) * 1024 * 1024)
                                    room = true;
                                else {
                                    ++video_budget_drops;
                                    discard_locked(image.frame_id, "per_chain_pixel_budget", generation);
                                }
                            }
                        }
                        if (!room) {
                            av_frame_unref(frame);
                            continue;
                        }
                        image.pixel_lease = ByteLease::reserve(app.video_budget, bytes);
                        if (!image.pixel_lease) {
                            std::lock_guard lock(mutex);
                            ++video_budget_drops;
                            discard_locked(image.frame_id, "global_pixel_budget", generation);
                            av_frame_unref(frame);
                            continue;
                        }
                        image.bgra.resize(bytes);
                        auto conversion_start = now_us();
                        if (!converter.convert(frame, image.width, image.height, image.stride, image.bgra))
                            throw std::runtime_error("video output conversion failed");
                        {
                            std::lock_guard lock(mutex);
                            conversion_ms = (now_us() - conversion_start) / 1000.0;
                            if (!stop_requested && generation == playback_generation) {
                                video_queue_bytes += bytes;
                                video_queue_peak_bytes = std::max(video_queue_peak_bytes, video_queue_bytes);
                                videos.push_back(std::move(image));
                            }
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
                        auto delay_samples = swr_get_delay(swr, frame->sample_rate);
                        int capacity = swr_get_out_samples(swr, frame->nb_samples);
                        AudioChunk chunk;
                        chunk.samples.resize(size_t(capacity) * 2);
                        uint8_t *out[] = {reinterpret_cast<uint8_t *>(chunk.samples.data())};
                        int n = swr_convert(swr, out, capacity,
                                            const_cast<const uint8_t **>(frame->extended_data),
                                            frame->nb_samples);
                        if (n > 0) {
                            chunk.samples.resize(size_t(n) * 2);
                            chunk.media_us = media - delay_samples * 1000000 / frame->sample_rate;
                            chunk.mapped = bool(u);
                            chunk.generation = generation;
                            chunk.due_us =
                                due - delay_samples * 1000000 / frame->sample_rate +
                                int64_t(f.audio_delay_ms) * 1000 +
                                int64_t(f.drift_ms_per_s * (now_us() - f.started_us) / 1000000.0) * 1000 +
                                int64_t(f.delay_ms) * 1000;
                            std::lock_guard lock(mutex);
                            if (generation != playback_generation || stop_requested) {
                                av_frame_unref(frame);
                                continue;
                            }
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
            reset_playback_locked("receiver_error");
            output_validity = "stale";
            audio_validity = "stale";
        }
        for (auto *&c : codecs)
            avcodec_free_context(&c);
        swr_free(&swr);
        av_frame_free(&frame);
        av_packet_free(&p);
        if (in)
            avformat_close_input(&in);
        if (!stop_requested)
            wait_until(now_us() + 1000000, stop_requested);
    }
}
void Chain::did_drop(uint64_t id, const std::string &reason, uint64_t generation) {
    std::lock_guard lock(mutex);
    discard_locked(id, reason, generation);
}
void Chain::did_present(uint64_t id, int64_t media, int64_t source_time, int64_t observed,
                        uint64_t generation, double render_ms) {
    std::lock_guard lock(mutex);
    if (stop_requested || generation != playback_generation) {
        observe({{"stage", "old_output_epoch_rejected"},
                 {"id", id},
                 {"generation", generation},
                 {"mono_us", observed}});
        return;
    }
    render_work_ms = render_ms;
    ++presented;
    if (last_present_us && observed - last_present_us > 500000) {
        sync_samples.clear();
        sync_step = {};
        ++sync_segment;
    }
    if (state == "running" && last_present_us && observed - last_present_us >= freeze.threshold_ms() * 1000) {
        completed_freezes.push_back(
            {{"start_mono_us", last_present_us},
             {"end_mono_us", observed},
             {"interval_ms", (observed - last_present_us) / 1000.0},
             {"threshold_ms", freeze.threshold_ms()},
             {"excess_ms", (observed - last_present_us) / 1000.0 - freeze.threshold_ms()}});
    }
    last_video_media = media;
    last_present_us = observed;
    last_video_source_valid = source_time > 0;
    if (!source_time)
        unmapped_outputs.push_back(observed);
    while (!unmapped_outputs.empty() && observed - unmapped_outputs.front() > 30000000)
        unmapped_outputs.pop_front();
    av_offset_ms.reset();
    if (source_time && audio_sample) {
        av_offset_ms = observed_sync_offset(*audio_sample, generation, media, observed);
        if (av_offset_ms &&
            append_sync_segment(sync_samples, {(observed - started_us) / 1000000.0, *av_offset_ms},
                                sync_step))
            ++sync_segment;
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
    } else
        latency_ms.reset();
    while (!latency_samples.empty() && observed - latency_samples.front().first > 1000000)
        latency_samples.pop_front();
    auto found = source_ids.find(id);
    if (found != source_ids.end())
        if (auto u = found->second.lock()) {
            u->frame.outcome = Outcome::presented;
            u->frame.present_us = observed;
        }
    output_validity = "software_render_complete";
    state = "running";
    observe({{"stage", "software_present"},
             {"id", id},
             {"generation", generation},
             {"mono_us", observed},
             {"media_us", media},
             {"source_us", source_time},
             {"sync_segment", sync_segment},
             {"audio_reference", av_offset_ms && audio_sample
                                     ? Json{{"generation", audio_sample->generation},
                                            {"media_us", audio_sample->media_us},
                                            {"observed_us", audio_sample->observed_us},
                                            {"valid_until_us", audio_sample->valid_until_us},
                                            {"device_position", audio_sample->device_position},
                                            {"device_frequency", audio_sample->device_frequency},
                                            {"pcm_begin_frame", audio_sample->pcm_begin_frame},
                                            {"pcm_begin_media_us", audio_sample->pcm_begin_media_us}}
                                     : Json(nullptr)},
             {"backend", "Direct2D_WIC_pixels_ready"}});
}
void Chain::audio_progress(const AudioClockSample &sample) {
    std::lock_guard lock(mutex);
    if (stop_requested || sample.generation != playback_generation ||
        sample.valid_until_us <= sample.observed_us)
        return;
    audio_sample = sample;
    audio_clock_media = sample.media_us;
    audio_clock_observed_us = sample.observed_us;
    audio_validity = "device_clock";
    if (sample.observed_us - last_audio_logged_us >= 20000) {
        observe({{"stage", "audio_device_position"},
                 {"media_us", sample.media_us},
                 {"mono_us", sample.observed_us},
                 {"device_position", sample.device_position},
                 {"generation", sample.generation},
                 {"valid_until_us", sample.valid_until_us},
                 {"device_frequency", sample.device_frequency}});
        last_audio_logged_us = sample.observed_us;
    }
}
void Chain::audio_unavailable(uint64_t generation, const std::string &reason) {
    std::lock_guard lock(mutex);
    if (generation != playback_generation)
        return;
    audio_sample.reset();
    audio_clock_media.reset();
    audio_clock_observed_us = 0;
    audio_validity = reason;
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
                auto ambiguous_output = std::any_of(unmapped_outputs.begin(), unmapped_outputs.end(),
                                                    [&](auto t) { return t >= u->source_us && t <= now; });
                f.outcome =
                    f.damaged && !ambiguous_output ? Outcome::transport_lost : Outcome::unknown_missing;
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
        size_t count = 0;
        for (auto [time, value] : latency_samples) {
            if (time > now || now - time > 1000000)
                continue;
            sum += value;
            ++count;
            mx = std::max(mx, value);
        }
        if (count) {
            latency_mean = sum / count;
            latency_max = mx;
        }
    }
    if (last_video_source_valid && audio_sample && now - audio_clock_observed_us < 100000 &&
        now - last_present_us < 500000) {
        std::vector<double> offsets;
        double t = (now - started_us) / 1000000.0;
        for (auto it = sync_samples.rbegin(); it != sync_samples.rend() && t - it->seconds <= 1; ++it)
            offsets.push_back(it->offset_ms);
        if (offsets.size() >= 3) {
            std::sort(offsets.begin(), offsets.end());
            sync_median = offsets[offsets.size() / 2];
        }
    }
    session_max_consecutive = std::max<uint64_t>(session_max_consecutive, loss.consecutive_max);
    uint64_t recent_bytes = 0;
    for (auto [time, bytes] : byte_samples)
        if (now - time <= 1000000)
            recent_bytes += bytes;
    std::string output_health = state == "running" ? (now - last_present_us >= 500000      ? "stalled"
                                                      : now - last_video_drop_us < 1000000 ? "dropping"
                                                                                           : "active")
                                                   : state;
    auto evidence = app.journal->stats(session);
    auto current_latency_count = std::count_if(latency_samples.begin(), latency_samples.end(), [&](auto &s) {
        return s.first <= now && now - s.first <= 1000000;
    });
    auto incomplete_latency_window = std::any_of(unmapped_outputs.begin(), unmapped_outputs.end(),
                                                 [&](auto t) { return t <= now && now - t <= 1000000; });
    Json j = {
        {"id", cfg.id},
        {"alias", cfg.alias},
        {"protocol", cfg.protocol},
        {"transport", cfg.transport},
        {"state", state == "running" && output_health != "active" ? "degraded" : state},
        {"connection_state", state},
        {"output_health", output_health},
        {"error", error},
        {"session", session},
        {"receiver_connection", receiver_connection},
        {"playback_generation", playback_generation},
        {"sync_segment", sync_segment},
        {"time_ms", utc_ms()},
        {"mono_us", now},
        {"width", width},
        {"height", height},
        {"fps", fps},
        {"audio_sample_rate", sample_rate},
        {"bitrate_kbps", recent_bytes * 8.0 / 1000},
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
        {"video_queue_bytes", video_queue_bytes},
        {"video_queue_peak_bytes", video_queue_peak_bytes},
        {"pixel_storage_bytes", app.video_budget->used()},
        {"pixel_storage_peak_bytes", app.video_budget->peak()},
        {"pixel_storage_limit_bytes", app.video_budget->limit()},
        {"output_width", settings.output_width},
        {"output_height", settings.output_height},
        {"video_budget_drops", video_budget_drops},
        {"video_late_drops", video_late_drops},
        {"output_lock_skips", output_lock_skips.load()},
        {"conversion_ms", conversion_ms},
        {"render_work_ms", render_work_ms},
        {"observation_queue_bytes", evidence.queued_bytes},
        {"observation_records_dropped", evidence.dropped},
        {"observation_records_written", evidence.written},
        {"evidence_validity",
         evidence.dropped ? "incomplete_raw_evidence" : "accepted_records_no_reported_loss"},
        {"video_due_lag_ms", videos.empty() ? 0.0 : (now - videos.front().due_us) / 1000.0},
        {"first_rx_media_us", first_rx_media},
        {"ambiguous", ambiguous},
        {"timestamp_recovered_frames", timestamp_recovered},
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
        {"latency_sample_count", current_latency_count},
        {"latency_validity",
         latency_mean ? (incomplete_latency_window ? "partial_association" : "valid")
                      : (now - last_present_us >= 500000 ? "output_stale" : "source_reference_unavailable")},
        {"av_offset_ms", sync_median ? Json(*sync_median) : Json(nullptr)},
        {"sync_validity", !cfg.has_audio             ? "not_applicable"
                          : !cfg.push_enabled        ? "source_reference_unavailable"
                          : sync_median              ? "valid"
                          : !last_video_source_valid ? "video_reference_unavailable"
                          : !audio_sample            ? audio_validity
                                                     : "insufficient_fresh_samples"},
        {"loss_pct", loss.percent ? Json(*loss.percent) : Json(nullptr)},
        {"loss_expected", loss.expected},
        {"loss_missing", loss.missing},
        {"loss_pending", loss.pending},
        {"loss_reasons", loss.reasons},
        {"damaged_presented_frames",
         std::count_if(recent.begin(), recent.end(),
                       [](auto &f) { return f.damaged && f.outcome == Outcome::presented; })},
        {"loss_unknown", loss.reasons.count("unknown_missing") ? loss.reasons.at("unknown_missing") : 0},
        {"loss_validity",
         loss.reasons.count("unknown_missing") ? "upper_bound_unassociated" : "resolved_source_frames"},
        {"loss_confirmed_pct", loss.expected ? Json(100.0 *
                                                    (loss.missing - (loss.reasons.count("unknown_missing")
                                                                         ? loss.reasons.at("unknown_missing")
                                                                         : 0)) /
                                                    loss.expected)
                                             : Json(nullptr)},
        {"consecutive_max", session_max_consecutive},
        {"window_consecutive_max", loss.consecutive_max},
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
    bool fit_fresh = !sync_samples.empty() &&
                     (now - started_us) / 1000000.0 - sync_samples.back().seconds <= 0.5 &&
                     sync_median.has_value();
    j["drift_ms_per_s"] =
        (!sync_step.pending && fit_fresh && fit && fit->residual_ms <= 15 && sync_samples.size() > 40 &&
         sync_samples.back().seconds - sync_samples.front().seconds > 48)
            ? Json(fit->slope_ms_per_s)
            : Json(nullptr);
    j["drift_validity"] = sync_step.pending ? "step_confirmation_pending"
                          : !fit_fresh      ? "stale_or_unmapped"
                          : !fit || sync_samples.back().seconds - sync_samples.front().seconds <= 48
                              ? "insufficient_continuous_span"
                          : fit->residual_ms > 15 ? "nonlinear_or_unstable"
                                                  : "valid";
    j["levels"] = Json::array();
    for (auto &a : alarms)
        j["levels"].push_back(level_name(a.level()));
    return j;
}
void Chain::tick() {
    auto j = snapshot();
    std::vector<Json> freezes;
    {
        std::lock_guard lock(mutex);
        freezes.swap(completed_freezes);
    }
    for (auto &event : freezes)
        app.event(cfg.id, "freeze", event);
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
    if (j["connection_state"] == "running")
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
                       {"thresholds",
                        {{"delay_warning_ms", settings.delay_warning_ms},
                         {"delay_critical_ms", settings.delay_critical_ms},
                         {"loss_warning_pct", settings.loss_warning_pct},
                         {"loss_critical_pct", settings.loss_critical_pct},
                         {"av_lead_warning_ms", settings.av_lead_warning_ms},
                         {"av_lag_warning_ms", settings.av_lag_warning_ms},
                         {"av_lead_critical_ms", settings.av_lead_critical_ms},
                         {"av_lag_critical_ms", settings.av_lag_critical_ms},
                         {"freeze_critical_ms", settings.freeze_critical_ms},
                         {"drift_warning_ms_per_s", settings.drift_warning_ms_per_s}}},
                       {"snapshot", j}});
    }
    app.store->sample(j);
}
} // namespace avqm
