#include "playback/preview.hpp"
#include <cstring>
#include <stdexcept>
#include <windows.h>
#include <wincodec.h>
#include <wrl/client.h>
using Microsoft::WRL::ComPtr;
namespace avqm {
void PreviewMirror::request(int64_t now) { lease_until_ = now + 2000000; }
bool PreviewMirror::wanted(int64_t now) const {
    return now < lease_until_.load() && now - captured_at_.load() >= 100000;
}
void PreviewMirror::capture(const uint8_t *pixels, int stride, int width, int height, uint64_t id,
                            uint64_t generation, int64_t observed) {
    if (!wanted(observed) || !pixels || width <= 0 || height <= 0 || width > 1000 || height > 600 ||
        stride < width * 4)
        return;
    auto frame = std::make_shared<PreviewFrame>();
    frame->width = width; frame->height = height;
    frame->frame_id = id; frame->generation = generation; frame->observed_us = observed;
    frame->pixels.resize(size_t(width) * height * 4);
    for (int row = 0; row < height; ++row)
        std::memcpy(frame->pixels.data() + size_t(row) * width * 4, pixels + size_t(row) * stride,
                    size_t(width) * 4);
    std::lock_guard lock(mutex_);
    frame->sequence = ++sequence_;
    raw_ = std::move(frame);
    captured_at_ = observed;
}
void PreviewMirror::clear() {
    std::lock_guard lock(mutex_);
    raw_.reset(); encoded_.reset(); captured_at_ = 0;
}
static void check(HRESULT value) {
    if (FAILED(value)) throw std::runtime_error("WIC preview JPEG encoding failed");
}
std::shared_ptr<const PreviewFrame> PreviewMirror::latest() {
    std::lock_guard encoding_lock(encoding_mutex_);
    std::shared_ptr<PreviewFrame> frame;
    {
        std::lock_guard lock(mutex_);
        if (!raw_) return {};
        if (encoded_ && encoded_->sequence == raw_->sequence) return encoded_;
        frame = raw_;
    }
    struct Apartment {
        HRESULT result = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        ~Apartment() { if (SUCCEEDED(result)) CoUninitialize(); }
    } apartment;
    if (FAILED(apartment.result) && apartment.result != RPC_E_CHANGED_MODE) check(apartment.result);
    ComPtr<IWICImagingFactory> factory;
    check(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory)));
    ComPtr<IWICBitmap> bitmap;
    check(factory->CreateBitmapFromMemory(frame->width, frame->height, GUID_WICPixelFormat32bppPBGRA,
                                          frame->width * 4, UINT(frame->pixels.size()), frame->pixels.data(), &bitmap));
    ComPtr<IWICFormatConverter> converter;
    check(factory->CreateFormatConverter(&converter));
    check(converter->Initialize(bitmap.Get(), GUID_WICPixelFormat24bppBGR, WICBitmapDitherTypeNone,
                                 nullptr, 0, WICBitmapPaletteTypeCustom));
    ComPtr<IStream> stream;
    check(CreateStreamOnHGlobal(nullptr, TRUE, &stream));
    ComPtr<IWICBitmapEncoder> encoder;
    check(factory->CreateEncoder(GUID_ContainerFormatJpeg, nullptr, &encoder));
    check(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache));
    ComPtr<IWICBitmapFrameEncode> output;
    ComPtr<IPropertyBag2> properties;
    check(encoder->CreateNewFrame(&output, &properties));
    check(output->Initialize(properties.Get()));
    check(output->SetSize(frame->width, frame->height));
    WICPixelFormatGUID format = GUID_WICPixelFormat24bppBGR;
    check(output->SetPixelFormat(&format));
    check(output->WriteSource(converter.Get(), nullptr));
    check(output->Commit()); check(encoder->Commit());
    STATSTG stat{};
    check(stream->Stat(&stat, STATFLAG_NONAME));
    if (stat.cbSize.QuadPart > 4000000) throw std::runtime_error("Preview JPEG exceeds bounded size");
    HGLOBAL memory{};
    check(GetHGlobalFromStream(stream.Get(), &memory));
    auto data = static_cast<const char *>(GlobalLock(memory));
    if (!data) throw std::runtime_error("Cannot read preview JPEG");
    frame->jpeg.assign(data, size_t(stat.cbSize.QuadPart));
    GlobalUnlock(memory);
    {
        std::lock_guard lock(mutex_);
        if (raw_ != frame) {
            // A new frame is fine, but never deliver a frame from before clear()/reconnect.
            if (!raw_ || raw_->generation != frame->generation) return {};
        }
        encoded_ = frame;
    }
    return frame;
}
} // namespace avqm
