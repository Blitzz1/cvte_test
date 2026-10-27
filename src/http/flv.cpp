#include "http/flv.hpp"
#include "media/chain.hpp"
extern "C" {
#include <libavformat/avformat.h>
}
namespace avqm {
namespace {
// This adapter only remuxes the ordinary encoded payload. Measurement remains in Chain.
struct FlvStream {
    Application &app;
    std::string source;
    AVFormatContext *input{};
    AVFormatContext *output{};
    AVPacket *packet = av_packet_alloc();
    AVIOContext *io{};
    httplib::DataSink *sink{};
    std::vector<int> streams;
    bool opened = false;
    FlvStream(Application &a, std::string url) : app(a), source(std::move(url)) {}
    static int interrupted(void *value) {
        return static_cast<FlvStream *>(value)->app.quit ? 1 : 0;
    }
    static int write(void *value, const uint8_t *data, int count) {
        auto &self = *static_cast<FlvStream *>(value);
        return self.sink && self.sink->write(reinterpret_cast<const char *>(data), size_t(count))
                   ? count
                   : AVERROR(EIO);
    }
    bool open() {
        input = avformat_alloc_context();
        input->interrupt_callback = {interrupted, this};
        AVDictionary *options{};
        av_dict_set(&options, "rw_timeout", "3000000", 0);
        av_dict_set(&options, "analyzeduration", "1000000", 0);
        av_dict_set(&options, "probesize", "1048576", 0);
        int rc = avformat_open_input(&input, source.c_str(), nullptr, &options);
        av_dict_free(&options);
        if (rc < 0 || avformat_find_stream_info(input, nullptr) < 0)
            return false;
        if (avformat_alloc_output_context2(&output, nullptr, "flv", nullptr) < 0)
            return false;
        streams.assign(input->nb_streams, -1);
        for (unsigned i = 0; i < input->nb_streams; i++) {
            auto *p = input->streams[i]->codecpar;
            if (p->codec_type != AVMEDIA_TYPE_VIDEO && p->codec_type != AVMEDIA_TYPE_AUDIO)
                continue;
            auto *stream = avformat_new_stream(output, nullptr);
            if (!stream)
                return false;
            if (avcodec_parameters_copy(stream->codecpar, p) < 0)
                return false;
            stream->codecpar->codec_tag = 0;
            stream->time_base = input->streams[i]->time_base;
            streams[i] = stream->index;
        }
        auto *buffer = static_cast<uint8_t *>(av_malloc(32768));
        if (!buffer)
            return false;
        io = avio_alloc_context(buffer, 32768, 1, this, nullptr, write, nullptr);
        if (!io) {
            av_free(buffer);
            return false;
        }
        output->pb = io;
        output->flags |= AVFMT_FLAG_CUSTOM_IO;
        if (avformat_write_header(output, nullptr) < 0)
            return false;
        avio_flush(io);
        opened = true;
        return true;
    }
    bool next(httplib::DataSink &target) {
        sink = &target;
        if (app.quit || (!opened && !open()))
            return false;
        if (av_read_frame(input, packet) < 0)
            return false;
        int idx = packet->stream_index;
        if (streams[idx] >= 0) {
            av_packet_rescale_ts(packet, input->streams[idx]->time_base,
                                 output->streams[streams[idx]]->time_base);
            packet->stream_index = streams[idx];
            packet->pos = -1;
            int rc = av_interleaved_write_frame(output, packet);
            av_packet_unref(packet);
            avio_flush(io);
            return rc >= 0 && !io->error;
        }
        av_packet_unref(packet);
        return true;
    }
    ~FlvStream() {
        av_packet_free(&packet);
        if (input)
            avformat_close_input(&input);
        if (output) {
            output->pb = nullptr;
            avformat_free_context(output);
        }
        if (io) {
            av_freep(&io->buffer);
            avio_context_free(&io);
        }
    }
};
} // namespace
void install_flv_route(httplib::Server &server, Application &app) {
    server.Get(R"(/flv/([^/]+)\.flv)", [&](const auto &req, auto &res) {
        auto chain = app.find(req.matches[1]);
        if (!chain) {
            res.status = 404;
            return;
        }
        std::string url;
        {
            std::lock_guard lock(chain->mutex);
            url = chain->cfg.publish_url;
        }
        if (url.empty()) {
            res.status = 400;
            return;
        }
        auto stream = std::make_shared<FlvStream>(app, url);
        res.set_header("Cache-Control", "no-store");
        res.set_chunked_content_provider(
            "video/x-flv", [stream](size_t, httplib::DataSink &sink) { return stream->next(sink); });
    });
}
} // namespace avqm
