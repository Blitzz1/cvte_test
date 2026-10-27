#pragma once
#include <cstddef>
#include <cstdint>
#include <span>
struct AVFrame;
struct SwsContext;
namespace avqm {
class VideoConverter {
  public:
    ~VideoConverter();
    bool convert(const AVFrame *source, int width, int height, size_t stride, std::span<uint8_t> destination);
    VideoConverter() = default;
    VideoConverter(const VideoConverter &) = delete;
    VideoConverter &operator=(const VideoConverter &) = delete;

  private:
    SwsContext *context_{};
};
} // namespace avqm
