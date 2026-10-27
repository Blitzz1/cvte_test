#include "playback/video_convert.hpp"
#include <limits>
extern "C" {
#include <libavutil/frame.h>
#include <libswscale/swscale.h>
}
namespace avqm {
VideoConverter::~VideoConverter() {
    sws_freeContext(context_);
}
bool VideoConverter::convert(const AVFrame *source, int width, int height, size_t stride,
                             std::span<uint8_t> destination) {
    if (!source || source->width <= 0 || source->height <= 0 || width <= 0 || height <= 0 ||
        size_t(width) > std::numeric_limits<size_t>::max() / 4 || stride < size_t(width) * 4 ||
        stride > size_t(std::numeric_limits<int>::max()) || size_t(height) > destination.size() / stride)
        return false;
    context_ =
        sws_getCachedContext(context_, source->width, source->height, AVPixelFormat(source->format), width,
                             height, AV_PIX_FMT_BGRA, SWS_FAST_BILINEAR, nullptr, nullptr, nullptr);
    if (!context_)
        return false;
    uint8_t *output[] = {destination.data()};
    int lines[] = {int(stride)};
    return sws_scale(context_, source->data, source->linesize, 0, source->height, output, lines) == height;
}
} // namespace avqm
