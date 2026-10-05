// Spike S1 — video wallpaper: cost of Media Foundation + D3D11 + DirectComposition, and
// attaching behind the desktop icons (docs/04, docs/09 §1-§3).
//
//   spike_s1.exe host [seconds]      SAFE on the host: plays in an ordinary bottom-most popup
//                                    window (NOT parented to Explorer) to measure RAM/CPU on the
//                                    real GPU.
//   spike_s1.exe desktop [seconds]   WINDOWS SANDBOX ONLY: attaches the window behind the desktop
//                                    icons (raised 24H2+ layout or legacy WorkerW) and verifies,
//                                    by reading screen pixels, that the video is visible.
//
// The test video (1080p30 H.264, 10 s) is generated locally with the Windows H.264 encoder.

#include "rmw/base/Utf.hpp"
#include "rmw/platform/Console.hpp"

#include <windows.h>

#include <d3d11.h>
#include <dcomp.h>
#include <dxgi1_3.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfmediaengine.h>
#include <mfreadwrite.h>
#include <wil/com.h>
#include <wil/resource.h>

#include <ole2.h>
#include <psapi.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <format>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

namespace
{

constexpr UINT kVideoWidth = 1920;
constexpr UINT kVideoHeight = 1080;
constexpr UINT kVideoFps = 30;
constexpr UINT kVideoSeconds = 10;

std::vector<std::string> g_items;

void item(const std::string& name, bool ok, const std::string& detail)
{
    std::string safe;
    for (char c : detail)
    {
        safe += (c == '"' || c == '\\') ? '/' : c;
    }
    g_items.push_back(
        std::format("{{\"name\":\"{}\",\"ok\":{},\"detail\":\"{}\"}}", name, ok ? "true" : "false", safe));
}

std::string hrText(HRESULT hr)
{
    return std::format("0x{:08X}", static_cast<unsigned>(hr));
}

// ------------------------------------------------------------------------------------------------
// Test video: 1080p30 H.264 with a moving gradient and a fixed magenta block (bottom-right) that
// the desktop test looks for on screen.
// ------------------------------------------------------------------------------------------------
HRESULT encodeTestVideo(const fs::path& path)
{
    wil::com_ptr<IMFAttributes> attributes;
    RETURN_IF_FAILED(MFCreateAttributes(&attributes, 1));
    RETURN_IF_FAILED(attributes->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE));
    wil::com_ptr<IMFSinkWriter> writer;
    RETURN_IF_FAILED(MFCreateSinkWriterFromURL(path.c_str(), nullptr, attributes.get(), &writer));

    wil::com_ptr<IMFMediaType> outType;
    RETURN_IF_FAILED(MFCreateMediaType(&outType));
    outType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    outType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
    outType->SetUINT32(MF_MT_AVG_BITRATE, 8'000'000);
    outType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    MFSetAttributeSize(outType.get(), MF_MT_FRAME_SIZE, kVideoWidth, kVideoHeight);
    MFSetAttributeRatio(outType.get(), MF_MT_FRAME_RATE, kVideoFps, 1);
    MFSetAttributeRatio(outType.get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    DWORD stream = 0;
    RETURN_IF_FAILED(writer->AddStream(outType.get(), &stream));

    wil::com_ptr<IMFMediaType> inType;
    RETURN_IF_FAILED(MFCreateMediaType(&inType));
    inType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    inType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
    inType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    inType->SetUINT32(MF_MT_DEFAULT_STRIDE, kVideoWidth * 4); // top-down
    MFSetAttributeSize(inType.get(), MF_MT_FRAME_SIZE, kVideoWidth, kVideoHeight);
    MFSetAttributeRatio(inType.get(), MF_MT_FRAME_RATE, kVideoFps, 1);
    MFSetAttributeRatio(inType.get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    RETURN_IF_FAILED(writer->SetInputMediaType(stream, inType.get(), nullptr));
    RETURN_IF_FAILED(writer->BeginWriting());

    constexpr DWORD frameBytes = kVideoWidth * kVideoHeight * 4;
    constexpr LONGLONG frameDuration = 10'000'000LL / kVideoFps;
    for (UINT frame = 0; frame < kVideoFps * kVideoSeconds; ++frame)
    {
        wil::com_ptr<IMFMediaBuffer> buffer;
        RETURN_IF_FAILED(MFCreateMemoryBuffer(frameBytes, &buffer));
        BYTE* data = nullptr;
        RETURN_IF_FAILED(buffer->Lock(&data, nullptr, nullptr));
        for (UINT y = 0; y < kVideoHeight; ++y)
        {
            BYTE* row = data + static_cast<size_t>(y) * kVideoWidth * 4;
            for (UINT x = 0; x < kVideoWidth; ++x)
            {
                BYTE* px = row + static_cast<size_t>(x) * 4;
                const bool block = x > kVideoWidth * 85 / 100 && y > kVideoHeight * 70 / 100;
                if (block)
                {
                    px[0] = 255; // B
                    px[1] = 0;   // G
                    px[2] = 255; // R
                }
                else
                {
                    px[0] = static_cast<BYTE>((x + frame * 8) & 0xFF);
                    px[1] = static_cast<BYTE>((y + frame * 4) & 0xFF);
                    px[2] = static_cast<BYTE>(frame * 2 & 0xFF);
                }
                px[3] = 255;
            }
        }
        buffer->Unlock();
        buffer->SetCurrentLength(frameBytes);

        wil::com_ptr<IMFSample> sample;
        RETURN_IF_FAILED(MFCreateSample(&sample));
        sample->AddBuffer(buffer.get());
        sample->SetSampleTime(frame * frameDuration);
        sample->SetSampleDuration(frameDuration);
        RETURN_IF_FAILED(writer->WriteSample(stream, sample.get()));
    }
    return writer->Finalize();
}

// ------------------------------------------------------------------------------------------------
// Media engine notifications
// ------------------------------------------------------------------------------------------------
class EngineNotify final : public IMFMediaEngineNotify
{
public:
    std::atomic<bool> canPlay{false};
    std::atomic<HRESULT> error{S_OK};

    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override
    {
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IMFMediaEngineNotify))
        {
            *ppv = static_cast<IMFMediaEngineNotify*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return ++m_refs; }
    STDMETHODIMP_(ULONG) Release() override
    {
        const ULONG refs = --m_refs;
        if (refs == 0)
        {
            delete this;
        }
        return refs;
    }
    STDMETHODIMP EventNotify(DWORD event, DWORD_PTR param1, DWORD param2) override
    {
        if (event == MF_MEDIA_ENGINE_EVENT_CANPLAY)
        {
            canPlay = true;
        }
        else if (event == MF_MEDIA_ENGINE_EVENT_ERROR)
        {
            error = static_cast<HRESULT>(param2);
            (void)param1;
        }
        return S_OK;
    }

private:
    std::atomic<ULONG> m_refs{1};
};

// ------------------------------------------------------------------------------------------------
// Process metrics
// ------------------------------------------------------------------------------------------------
struct Sample
{
    Clock::time_point when;
    ULONGLONG cpu100ns;
    SIZE_T privateBytes;
};

Sample sampleProcess()
{
    FILETIME created{}, exited{}, kernel{}, user{};
    GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user);
    const auto toU64 = [](FILETIME ft) {
        return (static_cast<ULONGLONG>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
    };
    PROCESS_MEMORY_COUNTERS_EX counters{};
    GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters),
                         sizeof(counters));
    return {Clock::now(), toU64(kernel) + toU64(user), counters.PrivateUsage};
}

double cpuPercent(const Sample& a, const Sample& b)
{
    const double wall100ns = std::chrono::duration<double>(b.when - a.when).count() * 1e7;
    SYSTEM_INFO info{};
    GetSystemInfo(&info);
    return wall100ns > 0
               ? 100.0 * static_cast<double>(b.cpu100ns - a.cpu100ns) / wall100ns / info.dwNumberOfProcessors
               : 0.0;
}

// ------------------------------------------------------------------------------------------------
// Window and desktop attach
// ------------------------------------------------------------------------------------------------
LRESULT CALLBACK windowProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_NCHITTEST)
    {
        return HTTRANSPARENT; // never steal clicks from the desktop
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

struct Attach
{
    bool ok = false;
    std::string detail;
};

// Algorithm from docs/09 §1 (Lively's approach, guided by Microsoft).
Attach attachBehindIcons(HWND window, const RECT& monitor)
{
    Attach result;
    const HWND progman = FindWindowW(L"Progman", nullptr);
    if (!progman)
    {
        result.detail = "Progman not found";
        return result;
    }
    const bool raised = (GetWindowLongPtrW(progman, GWL_EXSTYLE) & WS_EX_NOREDIRECTIONBITMAP) != 0;
    DWORD_PTR ignored = 0;
    SendMessageTimeoutW(progman, 0x052C, 0xD, 0x1, SMTO_NORMAL, 1000, &ignored);

    if (raised)
    {
        const HWND defView = FindWindowExW(progman, nullptr, L"SHELLDLL_DefView", nullptr);
        const HWND workerW = FindWindowExW(progman, nullptr, L"WorkerW", nullptr);
        SetWindowLongPtrW(window, GWL_STYLE, (GetWindowLongPtrW(window, GWL_STYLE) & ~WS_POPUP) | WS_CHILD);
        SetWindowLongPtrW(window, GWL_EXSTYLE, GetWindowLongPtrW(window, GWL_EXSTYLE) | WS_EX_LAYERED);
        SetLayeredWindowAttributes(window, 0, 255, LWA_ALPHA);
        SetParent(window, progman);
        POINT origin{monitor.left, monitor.top};
        MapWindowPoints(HWND_DESKTOP, progman, &origin, 1);
        SetWindowPos(window, defView, origin.x, origin.y, monitor.right - monitor.left,
                     monitor.bottom - monitor.top, SWP_NOACTIVATE | SWP_SHOWWINDOW);
        if (workerW)
        {
            SetWindowPos(workerW, HWND_BOTTOM, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        }
        const bool parentOk = GetParent(window) == progman;
        const bool belowIcons = GetWindow(window, GW_HWNDPREV) == defView;
        result.ok = parentOk && defView != nullptr;
        result.detail = std::format("layout=raised defView={} workerW={} parentOk={} directlyBelowIcons={}",
                                    defView != nullptr, workerW != nullptr, parentOk, belowIcons);
        return result;
    }

    // Legacy layout: the WorkerW after the top-level window that hosts SHELLDLL_DefView.
    HWND host = nullptr;
    EnumWindows(
        [](HWND top, LPARAM param) -> BOOL {
            if (FindWindowExW(top, nullptr, L"SHELLDLL_DefView", nullptr))
            {
                *reinterpret_cast<HWND*>(param) = top;
                return FALSE;
            }
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&host));
    const HWND workerW = host ? FindWindowExW(nullptr, host, L"WorkerW", nullptr) : nullptr;
    if (!workerW)
    {
        result.detail = "layout=legacy, WorkerW not found";
        return result;
    }
    SetWindowLongPtrW(window, GWL_STYLE, (GetWindowLongPtrW(window, GWL_STYLE) & ~WS_POPUP) | WS_CHILD);
    SetParent(window, workerW);
    POINT origin{monitor.left, monitor.top};
    MapWindowPoints(HWND_DESKTOP, workerW, &origin, 1);
    SetWindowPos(window, HWND_TOP, origin.x, origin.y, monitor.right - monitor.left,
                 monitor.bottom - monitor.top, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    result.ok = GetParent(window) == workerW;
    result.detail = std::format("layout=legacy parentOk={}", result.ok);
    return result;
}

COLORREF screenPixel(int x, int y)
{
    const HDC screen = GetDC(nullptr);
    const COLORREF color = GetPixel(screen, x, y);
    ReleaseDC(nullptr, screen);
    return color;
}

bool isMagenta(COLORREF c)
{
    return GetRValue(c) > 190 && GetGValue(c) < 80 && GetBValue(c) > 190;
}

} // namespace

int wmain(int argc, wchar_t** argv)
{
    const std::wstring_view mode = argc > 1 ? argv[1] : L"host";
    const int seconds = argc > 2 ? std::max(15, _wtoi(argv[2])) : 30;
    const bool desktopMode = mode == L"desktop";

    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    const auto coInit = wil::CoInitializeEx(COINIT_APARTMENTTHREADED);
    MFStartup(MF_VERSION);

    if (desktopMode)
    {
        for (int i = 0; i < 60 && !FindWindowW(L"Shell_TrayWnd", nullptr); ++i)
        {
            Sleep(1000);
        }
        Sleep(4000);
    }

    // 1. Test video ---------------------------------------------------------------------------
    const fs::path dir = fs::temp_directory_path() / "rmw-s1";
    fs::create_directories(dir);
    const fs::path video = dir / "test-1080p30.mp4";
    if (!fs::exists(video))
    {
        const auto start = Clock::now();
        const HRESULT hr = encodeTestVideo(video);
        item("encode:test-video", SUCCEEDED(hr),
             std::format("hr={} {}x{}@{} {}s in {:.1f}s size={}KB", hrText(hr), kVideoWidth, kVideoHeight,
                         kVideoFps, kVideoSeconds,
                         std::chrono::duration<double>(Clock::now() - start).count(),
                         fs::exists(video) ? fs::file_size(video) / 1024 : 0));
        if (FAILED(hr))
        {
            return 1;
        }
    }

    const Sample baseline = sampleProcess();

    // 2. Window --------------------------------------------------------------------------------
    MONITORINFO mi{.cbSize = sizeof(MONITORINFO)};
    GetMonitorInfoW(MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY), &mi);
    const RECT monitor = mi.rcMonitor;
    const UINT width = static_cast<UINT>(monitor.right - monitor.left);
    const UINT height = static_cast<UINT>(monitor.bottom - monitor.top);

    WNDCLASSEXW wc{.cbSize = sizeof(wc),
                   .lpfnWndProc = windowProc,
                   .hInstance = GetModuleHandleW(nullptr),
                   .hCursor = LoadCursorW(nullptr, IDC_ARROW),
                   .lpszClassName = L"RmwSpikeS1"};
    RegisterClassExW(&wc);
    wil::unique_hwnd window(CreateWindowExW(WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW | WS_EX_NOREDIRECTIONBITMAP,
                                            L"RmwSpikeS1", L"rmw spike s1", WS_POPUP, monitor.left,
                                            monitor.top, static_cast<int>(width), static_cast<int>(height),
                                            nullptr, nullptr, wc.hInstance, nullptr));
    COLORREF pixelBefore = 0;
    const int probeX = monitor.left + static_cast<int>(width * 92 / 100);
    const int probeY = monitor.top + static_cast<int>(height * 80 / 100);
    if (desktopMode)
    {
        pixelBefore = screenPixel(probeX, probeY);
        const Attach attach = attachBehindIcons(window.get(), monitor);
        item("desktop:attach", attach.ok, attach.detail);
    }
    else
    {
        SetWindowPos(window.get(), HWND_BOTTOM, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
    }

    // 3. D3D11 + DComp + media engine ---------------------------------------------------------
    wil::com_ptr<ID3D11Device> device;
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                                   D3D11_CREATE_DEVICE_VIDEO_SUPPORT | D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                                   levels, 2, D3D11_SDK_VERSION, &device, nullptr, nullptr);
    if (FAILED(hr))
    {
        item("d3d11:device", false, hrText(hr));
        return 1;
    }
    device.query<ID3D10Multithread>()->SetMultithreadProtected(TRUE);
    const Sample afterDevice = sampleProcess();
    auto dxgiDevice = device.query<IDXGIDevice>();
    wil::com_ptr<IDXGIAdapter> adapter;
    dxgiDevice->GetAdapter(&adapter);
    DXGI_ADAPTER_DESC adapterDesc{};
    adapter->GetDesc(&adapterDesc);
    item("d3d11:device", true, rmw::utf16To8Lossy(adapterDesc.Description));

    UINT resetToken = 0;
    wil::com_ptr<IMFDXGIDeviceManager> manager;
    MFCreateDXGIDeviceManager(&resetToken, &manager);
    manager->ResetDevice(device.get(), resetToken);

    wil::com_ptr<IDXGIFactory2> factory;
    adapter->GetParent(IID_PPV_ARGS(&factory));
    DXGI_SWAP_CHAIN_DESC1 desc{.Width = width,
                               .Height = height,
                               .Format = DXGI_FORMAT_B8G8R8A8_UNORM,
                               .SampleDesc = {1, 0},
                               .BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT,
                               .BufferCount = 2,
                               .Scaling = DXGI_SCALING_STRETCH,
                               .SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL,
                               .AlphaMode = DXGI_ALPHA_MODE_IGNORE};
    wil::com_ptr<IDXGISwapChain1> swapChain;
    hr = factory->CreateSwapChainForComposition(device.get(), &desc, nullptr, &swapChain);
    wil::com_ptr<IDCompositionDevice> dcomp;
    wil::com_ptr<IDCompositionTarget> target;
    wil::com_ptr<IDCompositionVisual> visual;
    if (SUCCEEDED(hr))
    {
        hr = DCompositionCreateDevice(dxgiDevice.get(), IID_PPV_ARGS(&dcomp));
    }
    if (SUCCEEDED(hr))
    {
        hr = dcomp->CreateTargetForHwnd(window.get(), TRUE, &target);
    }
    if (SUCCEEDED(hr))
    {
        dcomp->CreateVisual(&visual);
        visual->SetContent(swapChain.get());
        target->SetRoot(visual.get());
        hr = dcomp->Commit();
    }
    const Sample afterSwapChain = sampleProcess();
    item("dcomp:swapchain-for-composition", SUCCEEDED(hr), hrText(hr));
    if (FAILED(hr))
    {
        return 1;
    }

    auto* notify = new EngineNotify();
    wil::com_ptr<IMFMediaEngine> engine;
    {
        auto engineFactory = wil::CoCreateInstance<IMFMediaEngineClassFactory>(
            CLSID_MFMediaEngineClassFactory, CLSCTX_INPROC_SERVER);
        wil::com_ptr<IMFAttributes> attributes;
        MFCreateAttributes(&attributes, 3);
        attributes->SetUnknown(MF_MEDIA_ENGINE_DXGI_MANAGER, manager.get());
        attributes->SetUnknown(MF_MEDIA_ENGINE_CALLBACK, notify);
        attributes->SetUINT32(MF_MEDIA_ENGINE_VIDEO_OUTPUT_FORMAT, DXGI_FORMAT_B8G8R8A8_UNORM);
        hr = engineFactory->CreateInstance(0, attributes.get(), &engine);
    }
    notify->Release();
    const Sample afterEngine = sampleProcess();
    if (FAILED(hr))
    {
        item("mf:media-engine", false, hrText(hr));
        return 1;
    }
    BOOL canPlayMp4 = FALSE;
    const wil::unique_bstr mime(SysAllocString(L"video/mp4; codecs=\"avc1\""));
    MF_MEDIA_ENGINE_CANPLAY canPlay{};
    engine->CanPlayType(mime.get(), &canPlay);
    canPlayMp4 = canPlay != MF_MEDIA_ENGINE_CANPLAY_NOT_SUPPORTED;

    const wil::unique_bstr source(SysAllocString(video.c_str()));
    engine->SetMuted(TRUE);
    engine->SetLoop(TRUE);
    engine->SetSource(source.get());
    engine->Play();

    // 4. Render thread: present only when a new video frame exists, paced by vblank ---------------
    std::atomic<bool> stop{false};
    std::atomic<bool> paused{false};
    std::atomic<unsigned> presented{0};
    wil::com_ptr<IDXGIOutput> output;
    adapter->EnumOutputs(0, &output);
    std::jthread renderThread([&](std::stop_token) {
        while (!stop)
        {
            if (!output || FAILED(output->WaitForVBlank()))
            {
                Sleep(8);
            }
            if (paused)
            {
                continue;
            }
            LONGLONG pts = 0;
            if (engine->OnVideoStreamTick(&pts) == S_OK)
            {
                wil::com_ptr<ID3D11Texture2D> back;
                if (SUCCEEDED(swapChain->GetBuffer(0, IID_PPV_ARGS(&back))))
                {
                    const MFVideoNormalizedRect src{0.f, 0.f, 1.f, 1.f};
                    const RECT dst{0, 0, static_cast<LONG>(width), static_cast<LONG>(height)};
                    const MFARGB border{0, 0, 0, 255};
                    if (SUCCEEDED(engine->TransferVideoFrame(back.get(), &src, &dst, &border)))
                    {
                        swapChain->Present(1, 0);
                        ++presented;
                    }
                }
            }
        }
    });

    // 5. Phases: warm-up 5 s, measure playback, measure pause 10 s ---------------------------------
    const auto pump = [&](std::chrono::milliseconds duration) {
        const auto end = Clock::now() + duration;
        while (Clock::now() < end)
        {
            MsgWaitForMultipleObjects(0, nullptr, FALSE, 50, QS_ALLINPUT);
            MSG msg;
            while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
            {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
        }
    };

    pump(std::chrono::seconds(5));
    item("mf:playback-started", notify->canPlay && SUCCEEDED(notify->error.load()),
         std::format("canPlay={} error={} CanPlayType(mp4/avc1)={}", notify->canPlay.load(),
                     hrText(notify->error.load()), canPlayMp4));

    if (desktopMode)
    {
        const COLORREF after = screenPixel(probeX, probeY);
        item("desktop:video-visible-on-screen", isMagenta(after) && !isMagenta(pixelBefore),
             std::format("pixel({},{}) before=#{:02X}{:02X}{:02X} after=#{:02X}{:02X}{:02X}", probeX, probeY,
                         GetRValue(pixelBefore), GetGValue(pixelBefore), GetBValue(pixelBefore),
                         GetRValue(after), GetGValue(after), GetBValue(after)));
    }

    const Sample playStart = sampleProcess();
    item("memory:breakdown", true,
         std::format("start={:.1f}MB +d3d11Device={:.1f}MB +swapchain/dcomp={:.1f}MB +mediaEngine={:.1f}MB "
                     "+playing={:.1f}MB",
                     baseline.privateBytes / 1048576.0,
                     (afterDevice.privateBytes - baseline.privateBytes) / 1048576.0,
                     (afterSwapChain.privateBytes - afterDevice.privateBytes) / 1048576.0,
                     (afterEngine.privateBytes - afterSwapChain.privateBytes) / 1048576.0,
                     (playStart.privateBytes - afterEngine.privateBytes) / 1048576.0));
    const unsigned framesStart = presented;
    SIZE_T peakPrivate = playStart.privateBytes;
    const auto playSeconds = std::chrono::seconds(seconds - 15);
    const auto playEnd = Clock::now() + playSeconds;
    while (Clock::now() < playEnd)
    {
        pump(std::chrono::seconds(1));
        peakPrivate = std::max(peakPrivate, sampleProcess().privateBytes);
    }
    const Sample playStop = sampleProcess();
    const double playWall = std::chrono::duration<double>(playStop.when - playStart.when).count();
    const double fps = (presented - framesStart) / playWall;
    item("budget:playback-1080p30",
         playStop.privateBytes < 90ull * 1024 * 1024 && cpuPercent(playStart, playStop) <= 3.0,
         std::format(
             "cpu={:.2f}% private={:.1f}MB peak={:.1f}MB baselineBeforeD3D={:.1f}MB presentedFps={:.1f}",
             cpuPercent(playStart, playStop), playStop.privateBytes / 1048576.0, peakPrivate / 1048576.0,
             baseline.privateBytes / 1048576.0, fps));

    engine->Pause();
    paused = true;
    pump(std::chrono::seconds(2));
    const Sample pauseStart = sampleProcess();
    const unsigned framesPauseStart = presented;
    pump(std::chrono::seconds(8));
    const Sample pauseStop = sampleProcess();
    item("budget:paused", cpuPercent(pauseStart, pauseStop) < 0.5 && presented == framesPauseStart,
         std::format("cpu={:.2f}% framesWhilePaused={}", cpuPercent(pauseStart, pauseStop),
                     presented - framesPauseStart));

    stop = true;
    renderThread.join();
    engine->Shutdown();

    std::string json = "{\"items\":[";
    for (std::size_t i = 0; i < g_items.size(); ++i)
    {
        json += (i ? "," : "") + g_items[i];
    }
    json += "]}";
    rmw::platform::writeLine(rmw::platform::Stream::Out, json);
    MFShutdown();
    return 0;
}
