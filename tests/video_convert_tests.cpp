#include "playback/video_convert.hpp"
#include <algorithm>
#include <iostream>
#include <vector>
extern "C" {
#include <libavutil/frame.h>
#include <libavutil/pixfmt.h>
}
int main() {
    auto *frame = av_frame_alloc();
    frame->width = 1920;
    frame->height = 1080;
    frame->format = AV_PIX_FMT_YUV420P;
    if (av_frame_get_buffer(frame, 32) < 0)
        return 1;
    for (int plane = 0; plane < 3; ++plane) {
        int rows = plane ? 540 : 1080;
        for (int y = 0; y < rows; ++y)
            std::fill_n(frame->data[plane] + y * frame->linesize[plane], plane ? 960 : 1920,
                        uint8_t(plane ? 128 : 80));
    }
    avqm::VideoConverter converter;
    bool passed = true;
    for (auto [width, height] : {std::pair{320, 180}, std::pair{319, 179}}) {
        size_t stride = (size_t(width) * 4 + 31) & ~size_t(31), size = stride * height;
        std::vector<uint8_t> buffer(size + 64, 0xa5);
        auto pixels = std::span(buffer.data(), size);
        passed &= !converter.convert(frame, width, height, 1920 * 4, pixels);
        passed &= !converter.convert(frame, width, height, stride, pixels.first(size - 1));
        passed &= converter.convert(frame, width, height, stride, pixels);
        passed &= buffer[(height - 1) * stride + (width - 1) * 4 + 3] == 255;
        passed &= std::all_of(buffer.begin() + size, buffer.end(), [](auto b) { return b == 0xa5; });
    }
    av_frame_free(&frame);
    std::cout << (passed ? "Video conversion bounds and guard bytes passed\n"
                         : "Video conversion bounds failed\n");
    return passed ? 0 : 1;
}
