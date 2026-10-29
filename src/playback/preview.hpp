#pragma once
#include <algorithm>
#include <cmath>
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>
namespace avqm {
struct PreviewRect { int x{}, y{}, width{}, height{}; };
// Share the exact integer WIC drawing rectangle with the pixel mirror.
inline PreviewRect preview_content_rect(int x, int y, int width, int height, int video_width, int video_height) {
    if (width <= 0 || height <= 0 || video_width <= 0 || video_height <= 0) return {};
    const double scale = std::min(double(width) / video_width, double(height) / video_height);
    const int w = std::clamp(int(std::lround(video_width * scale)), 1, width);
    const int h = std::clamp(int(std::lround(video_height * scale)), 1, height);
    return {x + (width - w) / 2, y + (height - h) / 2, w, h};
}
struct PreviewFrame {
    int width{}, height{};
    uint64_t sequence{}, frame_id{}, generation{};
    int64_t observed_us{};
    std::vector<uint8_t> pixels;
    std::string jpeg;
};
class PreviewMirror {
  public:
    void request(int64_t now);
    bool wanted(int64_t now) const;
    void capture(const uint8_t *pixels, int stride, int width, int height, uint64_t frame_id,
                 uint64_t generation, int64_t observed);
    void clear();
    std::shared_ptr<const PreviewFrame> latest();
  private:
    std::atomic<int64_t> lease_until_{0}, captured_at_{0};
    std::mutex mutex_, encoding_mutex_;
    uint64_t sequence_{};
    std::shared_ptr<PreviewFrame> raw_, encoded_;
};
} // namespace avqm
