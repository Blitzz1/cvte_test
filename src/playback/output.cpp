#include "playback/output.hpp"
#include "media/chain.hpp"
#include <audioclient.h>
#include <d2d1.h>
#include <dwmapi.h>
#include <mmdeviceapi.h>
#include <wincodec.h>
#include <windows.h>
#include <wrl/client.h>
using Microsoft::WRL::ComPtr;
namespace avqm {
struct Output::Impl {
    Application &app;
    HWND window{};
    ComPtr<ID2D1Factory> factory;
    ComPtr<ID2D1RenderTarget> renderer;
    ComPtr<IWICImagingFactory> wic;
    ComPtr<IWICBitmap> surface;
    ComPtr<ID2D1SolidColorBrush> black;
    std::vector<ComPtr<ID2D1Bitmap>> bitmaps;
    std::atomic<int> selected_audio{-1};
    std::thread audio_thread;
    std::atomic<bool> done{false};
    explicit Impl(Application &a) : app(a) {}
    static LRESULT CALLBACK window_proc(HWND w, UINT msg, WPARAM wp, LPARAM lp) {
        if (msg == WM_CLOSE) {
            DestroyWindow(w);
            return 0;
        }
        if (msg == WM_DESTROY) {
            PostQuitMessage(0);
            return 0;
        }
        return DefWindowProc(w, msg, wp, lp);
    }
    struct AudioDevice {
        ComPtr<IMMDeviceEnumerator> enumerator;
        ComPtr<IMMDevice> device;
        ComPtr<IAudioClient> client;
        ComPtr<IAudioRenderClient> render;
        ComPtr<IAudioClock> clock;
        ComPtr<ISimpleAudioVolume> volume;
        UINT32 capacity{};
        UINT64 frequency{};
        uint64_t submitted{};
        uint64_t last_position{};
        int64_t last_position_us{};
        uint64_t generation = 0;
        int last_audio_selection = -2;
        struct Segment {
            uint64_t begin{}, end{};
            int64_t media{};
        };
        std::deque<Segment> segments;
        std::optional<AudioChunk> chunk;
        size_t offset{};
        bool init() {
            if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                        IID_PPV_ARGS(&enumerator))))
                return false;
            if (FAILED(enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device)))
                return false;
            if (FAILED(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &client)))
                return false;
            WAVEFORMATEX f{};
            f.wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
            f.nChannels = 2;
            f.nSamplesPerSec = 48000;
            f.wBitsPerSample = 32;
            f.nBlockAlign = 8;
            f.nAvgBytesPerSec = 384000;
            GUID audio_session{};
            CoCreateGuid(&audio_session);
            if (FAILED(client->Initialize(AUDCLNT_SHAREMODE_SHARED,
                                          AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
                                              AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
                                          1000000, 0, &f, &audio_session)))
                return false;
            if (FAILED(client->GetBufferSize(&capacity)) ||
                FAILED(client->GetService(IID_PPV_ARGS(&render))) ||
                FAILED(client->GetService(IID_PPV_ARGS(&clock))))
                return false;
            client->GetService(IID_PPV_ARGS(&volume));
            if (FAILED(clock->GetFrequency(&frequency)))
                return false;
            BYTE *initial{};
            if (FAILED(render->GetBuffer(capacity, &initial)))
                return false;
            std::memset(initial, 0, size_t(capacity) * 8);
            render->ReleaseBuffer(capacity, AUDCLNT_BUFFERFLAGS_SILENT);
            submitted = capacity;
            return SUCCEEDED(client->Start());
        }
        ~AudioDevice() {
            if (client)
                client->Stop();
        }
    };
    void audio_loop() {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        std::vector<std::unique_ptr<AudioDevice>> devices(app.chains.size());
        std::vector<int64_t> retry_at(app.chains.size(), 0);
        while (!done && !app.quit) {
            for (size_t i = 0; i < devices.size(); ++i) {
                auto &chain = *app.chains[i];
                uint64_t generation;
                bool needed;
                {
                    std::lock_guard lock(chain.mutex);
                    generation = chain.playback_generation;
                    needed = chain.cfg.has_audio && !chain.stop_requested;
                }
                if (!needed) {
                    devices[i].reset();
                    continue;
                }
                if (!devices[i]) {
                    if (now_us() < retry_at[i])
                        continue;
                    auto fresh = std::make_unique<AudioDevice>();
                    if (!fresh->init()) {
                        chain.audio_unavailable(generation, "audio_device_unavailable");
                        retry_at[i] = now_us() + 1000000;
                        continue;
                    }
                    fresh->generation = generation;
                    devices[i] = std::move(fresh);
                }
                auto &d = *devices[i];
                // Device calls never hold the media mutex.
                if (d.generation != generation) {
                    d.client->Stop();
                    if (FAILED(d.client->Reset())) {
                        chain.audio_unavailable(generation, "audio_reset_failed");
                        devices[i].reset();
                        retry_at[i] = now_us() + 1000000;
                        continue;
                    }
                    BYTE *initial{};
                    if (FAILED(d.render->GetBuffer(d.capacity, &initial))) {
                        chain.audio_unavailable(generation, "audio_reset_failed");
                        devices[i].reset();
                        continue;
                    }
                    std::memset(initial, 0, size_t(d.capacity) * 8);
                    d.render->ReleaseBuffer(d.capacity, AUDCLNT_BUFFERFLAGS_SILENT);
                    if (FAILED(d.client->Start())) {
                        chain.audio_unavailable(generation, "audio_start_failed");
                        devices[i].reset();
                        continue;
                    }
                    d.generation = generation;
                    d.submitted = d.capacity;
                    d.segments.clear();
                    d.chunk.reset();
                    d.offset = 0;
                    d.last_position = 0;
                    d.last_position_us = 0;
                }
                if (d.volume && d.last_audio_selection != selected_audio) {
                    d.volume->SetMasterVolume(selected_audio == int(i) ? 0.4f : 0.0f, nullptr);
                    d.last_audio_selection = selected_audio;
                }
                UINT32 padding{};
                if (FAILED(d.client->GetCurrentPadding(&padding)) || padding > d.capacity) {
                    chain.audio_unavailable(generation, "audio_device_error");
                    devices[i].reset();
                    retry_at[i] = now_us() + 1000000;
                    continue;
                }
                if (padding == 0) {
                    UINT64 raw{}, qpc{};
                    if (d.clock->GetPosition(&raw, &qpc) == S_OK && d.frequency) {
                        auto current = uint64_t((long double)raw * 48000 / d.frequency);
                        if (current > d.submitted) {
                            d.submitted = current;
                            d.segments.clear();
                            chain.audio_unavailable(generation, "audio_underrun");
                        }
                    }
                }
                auto available = d.capacity - padding;
                if (available) {
                    BYTE *bytes{};
                    if (SUCCEEDED(d.render->GetBuffer(available, &bytes))) {
                        auto *out = reinterpret_cast<float *>(bytes);
                        std::fill(out, out + size_t(available) * 2, 0.0f);
                        size_t written = 0;
                        while (written < available) {
                            if (!d.chunk) {
                                std::lock_guard lock(chain.mutex);
                                if (generation != chain.playback_generation || chain.stop_requested)
                                    break;
                                auto target = now_us() + int64_t(padding + written) * 1000000 / 48000;
                                while (
                                    !chain.audio.empty() &&
                                    (chain.audio.front().generation != generation ||
                                     (chain.audio.size() > 1 && chain.audio.front().due_us < target - 15000)))
                                    chain.audio.pop_front();
                                if (chain.audio.empty() || chain.audio.front().due_us > target)
                                    break;
                                d.chunk = std::move(chain.audio.front());
                                chain.audio.pop_front();
                                d.offset = 0;
                            }
                            if (d.chunk->generation != generation) {
                                d.chunk.reset();
                                continue;
                            }
                            auto remaining = d.chunk->samples.size() / 2 - d.offset;
                            auto count = std::min<size_t>(available - written, remaining);
                            std::copy_n(d.chunk->samples.data() + d.offset * 2, count * 2, out + written * 2);
                            if (d.chunk->mapped)
                                d.segments.push_back(
                                    {d.submitted + written, d.submitted + written + count,
                                     d.chunk->media_us + int64_t(d.offset) * 1000000 / 48000});
                            written += count;
                            d.offset += count;
                            if (d.offset >= d.chunk->samples.size() / 2)
                                d.chunk.reset();
                        }
                        if (FAILED(d.render->ReleaseBuffer(available, 0))) {
                            chain.audio_unavailable(generation, "audio_device_error");
                            devices[i].reset();
                            continue;
                        }
                        d.submitted += available;
                    } else {
                        chain.audio_unavailable(generation, "audio_buffer_unavailable");
                        continue;
                    }
                }
                UINT64 position{}, qpc{};
                bool valid = false;
                if (d.clock->GetPosition(&position, &qpc) == S_OK && d.frequency) {
                    if ((d.last_position_us && position < d.last_position) ||
                        (d.last_position_us && position == d.last_position &&
                         now_us() - d.last_position_us > 100000) ||
                        d.segments.size() > 512) {
                        chain.audio_unavailable(generation, "audio_clock_discontinuous");
                        devices[i].reset();
                        retry_at[i] = now_us() + 1000000;
                        continue;
                    }
                    if (!d.last_position_us || position != d.last_position) {
                        d.last_position = position;
                        d.last_position_us = now_us();
                    }
                    auto frame = uint64_t((long double)position * 48000 / d.frequency);
                    while (!d.segments.empty() && d.segments.front().end <= frame)
                        d.segments.pop_front();
                    if (!d.segments.empty() && frame >= d.segments.front().begin &&
                        frame < d.segments.front().end) {
                        auto &segment = d.segments.front();
                        LARGE_INTEGER counter{}, frequency{};
                        QueryPerformanceCounter(&counter);
                        QueryPerformanceFrequency(&frequency);
                        auto age = int64_t((long double)counter.QuadPart * 1000000 / frequency.QuadPart) -
                                   int64_t(qpc / 10);
                        if (age >= 0 && age < 15000) {
                            auto observed = now_us() - age;
                            chain.audio_progress(AudioClockSample{
                                generation, segment.media + int64_t(frame - segment.begin) * 1000000 / 48000,
                                observed, observed + int64_t(segment.end - frame) * 1000000 / 48000, position,
                                d.frequency, segment.begin, segment.media});
                            valid = true;
                        }
                    }
                }
                if (!valid)
                    chain.audio_unavailable(generation, "silence_or_unmapped_audio");
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        devices.clear();
        CoUninitialize();
    }
};
Output::Output(Application &a) : p_(std::make_unique<Impl>(a)) {}
Output::~Output() {
    p_->done = true;
    if (p_->audio_thread.joinable())
        p_->audio_thread.join();
}
void Output::select_audio(int i) {
    p_->selected_audio = i;
}
void Output::run(int seconds) {
    auto &p = *p_;
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    auto instance = GetModuleHandle(nullptr);
    WNDCLASS wc{};
    wc.lpfnWndProc = Impl::window_proc;
    wc.hInstance = instance;
    wc.lpszClassName = L"AVQMOutput";
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    RegisterClass(&wc);
    p.window = CreateWindowEx(0, wc.lpszClassName, L"AVQualityMonitor - software playback observation",
                              WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 1000, 650, nullptr, nullptr,
                              instance, nullptr);
    if (!p.window)
        throw std::runtime_error("Cannot create monitor output window");
    if (p.app.config.value("show_monitor", false)) {
        ShowWindow(p.window, SW_SHOWNOACTIVATE);
        ShowWindow(p.window, SW_SHOWNOACTIVATE);
    }
    p.bitmaps.resize(p.app.chains.size());
    if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, p.factory.GetAddressOf())))
        throw std::runtime_error("Direct2D factory creation failed");
    if (FAILED(
            CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&p.wic))))
        throw std::runtime_error("WIC output initialization failed");
    if (FAILED(
            p.wic->CreateBitmap(1000, 600, GUID_WICPixelFormat32bppPBGRA, WICBitmapCacheOnLoad, &p.surface)))
        throw std::runtime_error("Software output surface allocation failed");
    if (FAILED(p.factory->CreateWicBitmapRenderTarget(
            p.surface.Get(), D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_SOFTWARE),
            p.renderer.GetAddressOf())))
        throw std::runtime_error("Direct2D output initialization failed");
    if (FAILED(p.renderer->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::Black), &p.black)))
        throw std::runtime_error("output brush creation failed");
    p.renderer->BeginDraw();
    p.renderer->Clear(D2D1::ColorF(D2D1::ColorF::Black));
    if (FAILED(p.renderer->EndDraw()))
        throw std::runtime_error("output surface initialization failed");
    p.audio_thread = std::thread([&p] { p.audio_loop(); });
    auto started = now_us();
    while (!p.app.quit) {
        MSG message;
        while (PeekMessage(&message, nullptr, 0, 0, PM_REMOVE)) {
            if (message.message == WM_QUIT)
                p.app.quit = true;
            TranslateMessage(&message);
            DispatchMessage(&message);
        }
        if (seconds && now_us() - started > seconds * 1000000LL)
            p.app.quit = true;
        if (p.app.quit)
            break;
        bool changed = false;
        std::vector<std::pair<size_t, VideoImage>> updates;
        auto current = now_us();
        for (size_t i = 0; i < p.app.chains.size(); ++i) {
            auto &c = *p.app.chains[i];
            std::unique_lock lock(c.mutex, std::try_to_lock);
            if (!lock.owns_lock()) {
                ++c.output_lock_skips;
                continue;
            }
            while (!c.videos.empty() && (c.videos.front().generation != c.playback_generation ||
                                         c.videos.front().due_us < current - 100000)) {
                auto id = c.videos.front().frame_id;
                c.discard_locked(id, "output_deadline_missed", c.videos.front().generation);
                ++c.video_late_drops;
                c.video_queue_bytes -= c.videos.front().bgra.size();
                c.videos.pop_front();
            }
            if (!c.videos.empty() && c.videos.front().due_us <= current) {
                auto image = std::move(c.videos.front());
                c.video_queue_bytes -= image.bgra.size();
                c.videos.pop_front();
                updates.emplace_back(i, std::move(image));
                changed = true;
            }
        }
        if (changed) {
            auto render_start = now_us();
            std::vector<bool> painted(p.app.chains.size(), false);
            p.renderer->BeginDraw();
            int cols = p.app.chains.size() > 4 ? 4 : 2;
            int rows = int((p.app.chains.size() + cols - 1) / cols);
            int w = 1000 / cols, h = 600 / std::max(1, rows);
            for (auto &[i, v] : updates) {
                if (!p.bitmaps[i] || p.bitmaps[i]->GetPixelSize().width != UINT32(v.width) ||
                    p.bitmaps[i]->GetPixelSize().height != UINT32(v.height)) {
                    p.bitmaps[i].Reset();
                    p.renderer->CreateBitmap(D2D1::SizeU(v.width, v.height), nullptr, 0,
                                             D2D1::BitmapProperties(D2D1::PixelFormat(
                                                 DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE)),
                                             p.bitmaps[i].GetAddressOf());
                }
                if (p.bitmaps[i] &&
                    SUCCEEDED(p.bitmaps[i]->CopyFromMemory(nullptr, v.bgra.data(), v.stride))) {
                    auto x = float(i % cols * w), y = float(i / cols * h);
                    p.renderer->FillRectangle(D2D1::RectF(x, y, x + w, y + h), p.black.Get());
                    auto scale = std::min(float(w) / v.width, float(h) / v.height);
                    auto rw = v.width * scale, rh = v.height * scale;
                    p.renderer->DrawBitmap(
                        p.bitmaps[i].Get(),
                        D2D1::RectF(x + (w - rw) / 2, y + (h - rh) / 2, x + (w + rw) / 2, y + (h + rh) / 2));
                    painted[i] = true;
                } else
                    p.app.chains[i]->did_drop(v.frame_id, "output_bitmap_failed", v.generation);
            }
            auto result = p.renderer->EndDraw();
            // Locking the WIC-backed software target confirms actual rendered pixels are available.
            ComPtr<IWICBitmapLock> pixels;
            WICRect bounds{0, 0, 1000, 600};
            if (SUCCEEDED(result))
                result = p.surface->Lock(&bounds, WICBitmapLockRead, &pixels);
            auto observed = now_us();
            for (auto &[i, v] : updates) {
                if (!painted[i])
                    continue;
                if (SUCCEEDED(result))
                    p.app.chains[i]->did_present(v.frame_id, v.media_us, v.source_us, observed, v.generation,
                                                 (observed - render_start) / 1000.0);
                else
                    p.app.chains[i]->did_drop(v.frame_id, "output_backend_failed", v.generation);
                if (SUCCEEDED(result) && p.app.chains[i]->preview.wanted(observed)) {
                    UINT bytes{}, stride{};
                    BYTE *data{};
                    if (SUCCEEDED(pixels->GetStride(&stride)) &&
                        SUCCEEDED(pixels->GetDataPointer(&bytes, &data)) && bytes >= stride * 600) {
                        auto &chain = *p.app.chains[i];
                        std::lock_guard lock(chain.mutex);
                        if (!chain.stop_requested && chain.playback_generation == v.generation)
                            chain.preview.capture(data + (i / cols * h) * stride + (i % cols * w) * 4,
                                                   int(stride), w, h, v.frame_id, v.generation, observed);
                    }
                }
            }
            if (pixels && !IsIconic(p.window) && IsWindowVisible(p.window)) {
                UINT bytes{}, stride{};
                BYTE *data{};
                pixels->GetDataPointer(&bytes, &data);
                pixels->GetStride(&stride);
                BITMAPINFO info{};
                info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
                info.bmiHeader.biWidth = int(stride / 4);
                info.bmiHeader.biHeight = -600;
                info.bmiHeader.biPlanes = 1;
                info.bmiHeader.biBitCount = 32;
                info.bmiHeader.biCompression = BI_RGB;
                RECT rect;
                GetClientRect(p.window, &rect);
                auto dc = GetDC(p.window);
                StretchDIBits(dc, 0, 0, rect.right, rect.bottom, 0, 0, 1000, 600, data, &info, DIB_RGB_COLORS,
                              SRCCOPY);
                ReleaseDC(p.window, dc);
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(3));
    }
    p.done = true;
    if (p.audio_thread.joinable())
        p.audio_thread.join();
    DestroyWindow(p.window);
    p.bitmaps.clear();
    p.black.Reset();
    p.renderer.Reset();
    p.surface.Reset();
    p.wic.Reset();
    p.factory.Reset();
    CoUninitialize();
}
} // namespace avqm
