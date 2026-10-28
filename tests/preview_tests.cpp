#include "playback/preview.hpp"
#include <iostream>
#include <stdexcept>
using namespace avqm;
static void require(bool value, const char *message) {
    if (!value) throw std::runtime_error(message);
}
int main() {
    PreviewMirror preview;
    std::vector<uint8_t> pixels(32 * 16 * 4, 255);
    preview.capture(pixels.data(), 128, 32, 16, 42, 1, 1000000);
    require(!preview.latest(), "Do not capture without an active preview lease");
    preview.request(1000000);
    preview.capture(pixels.data(), 128, 32, 16, 42, 1, 1000000);
    auto first = preview.latest();
    require(first && first->frame_id == 42 && first->generation == 1, "Preserve real output identity");
    require(first->width == 32 && first->height == 16 && first->observed_us == 1000000,
            "Preserve actual output dimensions and time");
    require(first->jpeg.size() > 4 && uint8_t(first->jpeg[0]) == 255 && uint8_t(first->jpeg[1]) == 216,
            "WIC must produce a real JPEG");
    require(preview.latest() == first, "Reuse the encoded last actual frame when output freezes");
    preview.capture(pixels.data(), 128, 32, 16, 43, 1, 1050000);
    require(preview.latest() == first, "Throttle copying to ten frames per second");
    require(!preview.wanted(3000001), "Expire unused preview leases");
    preview.clear();
    require(!preview.latest(), "Clear old picture on stop or reconnect");
    preview.request(4000000);
    preview.capture(pixels.data(), 128, 32, 16, 1, 2, 4000000);
    auto next = preview.latest();
    require(next && next->generation == 2 && next->sequence > first->sequence, "Isolate new playback generations");
    std::cout << "Preview lease, real JPEG, frame identity, freeze, bounded copying and reconnect passed\n";
}
