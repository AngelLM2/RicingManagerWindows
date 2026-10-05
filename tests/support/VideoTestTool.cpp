// rmw_video_test_tool.exe — helpers for the M6 system tests (tests/system/sandbox/video-tests.ps1):
//   encode <file.mp4> [seconds]   writes a 1920x1080, 30 fps H.264 video with Windows' own encoder:
//                                 a moving gradient and a fixed magenta block in the bottom-right
//                                 corner (the test looks for it on screen)
//   fullscreen                    a borderless window over the whole primary monitor, in front
//   cover                         a normal window, maximized on the primary monitor
// The windows stay until the process is ended.

#include <windows.h>

#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <wil/com.h>
#include <wil/resource.h>

#include <cstdio>
#include <cwchar>
#include <string_view>

namespace
{

constexpr UINT32 kWidth = 1920;
constexpr UINT32 kHeight = 1080;
constexpr UINT32 kFps = 30;

HRESULT encode(const wchar_t* path, UINT32 seconds)
{
    wil::com_ptr_nothrow<IMFAttributes> attributes;
    RETURN_IF_FAILED(MFCreateAttributes(attributes.put(), 1));
    RETURN_IF_FAILED(attributes->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE));
    wil::com_ptr_nothrow<IMFSinkWriter> writer;
    RETURN_IF_FAILED(MFCreateSinkWriterFromURL(path, nullptr, attributes.get(), writer.put()));

    wil::com_ptr_nothrow<IMFMediaType> output;
    RETURN_IF_FAILED(MFCreateMediaType(output.put()));
    output->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    output->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
    output->SetUINT32(MF_MT_AVG_BITRATE, 6'000'000);
    output->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    MFSetAttributeSize(output.get(), MF_MT_FRAME_SIZE, kWidth, kHeight);
    MFSetAttributeRatio(output.get(), MF_MT_FRAME_RATE, kFps, 1);
    MFSetAttributeRatio(output.get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    DWORD stream = 0;
    RETURN_IF_FAILED(writer->AddStream(output.get(), &stream));

    wil::com_ptr_nothrow<IMFMediaType> input;
    RETURN_IF_FAILED(MFCreateMediaType(input.put()));
    input->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    input->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
    input->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    input->SetUINT32(MF_MT_DEFAULT_STRIDE, kWidth * 4); // top-down
    MFSetAttributeSize(input.get(), MF_MT_FRAME_SIZE, kWidth, kHeight);
    MFSetAttributeRatio(input.get(), MF_MT_FRAME_RATE, kFps, 1);
    MFSetAttributeRatio(input.get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    RETURN_IF_FAILED(writer->SetInputMediaType(stream, input.get(), nullptr));
    RETURN_IF_FAILED(writer->BeginWriting());

    constexpr DWORD frameBytes = kWidth * kHeight * 4;
    constexpr LONGLONG frameDuration = 10'000'000LL / kFps;
    for (UINT32 frame = 0; frame < kFps * seconds; ++frame)
    {
        wil::com_ptr_nothrow<IMFMediaBuffer> buffer;
        RETURN_IF_FAILED(MFCreateMemoryBuffer(frameBytes, buffer.put()));
        BYTE* data = nullptr;
        RETURN_IF_FAILED(buffer->Lock(&data, nullptr, nullptr));
        for (UINT32 y = 0; y < kHeight; ++y)
        {
            BYTE* row = data + (static_cast<std::size_t>(y) * kWidth * 4);
            for (UINT32 x = 0; x < kWidth; ++x)
            {
                BYTE* pixel = row + (static_cast<std::size_t>(x) * 4);
                const bool block = x > kWidth * 85 / 100 && y > kHeight * 70 / 100;
                pixel[0] = block ? 255 : static_cast<BYTE>((x + (frame * 8)) & 0xFF); // B
                pixel[1] = block ? 0 : static_cast<BYTE>((y + (frame * 4)) & 0xFF);   // G
                pixel[2] = block ? 255 : static_cast<BYTE>((frame * 2) & 0xFF);       // R
                pixel[3] = 255;
            }
        }
        buffer->Unlock();
        buffer->SetCurrentLength(frameBytes);
        wil::com_ptr_nothrow<IMFSample> sample;
        RETURN_IF_FAILED(MFCreateSample(sample.put()));
        sample->AddBuffer(buffer.get());
        sample->SetSampleTime(frame * frameDuration);
        sample->SetSampleDuration(frameDuration);
        RETURN_IF_FAILED(writer->WriteSample(stream, sample.get()));
    }
    return writer->Finalize();
}

LRESULT CALLBACK windowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
    if (message == WM_PAINT)
    {
        PAINTSTRUCT paint{};
        HDC dc = BeginPaint(window, &paint);
        FillRect(dc, &paint.rcPaint, static_cast<HBRUSH>(GetStockObject(DKGRAY_BRUSH)));
        EndPaint(window, &paint);
        return 0;
    }
    if (message == WM_DESTROY)
    {
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

int showWindow(bool fullscreen)
{
    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.lpfnWndProc = &windowProc;
    windowClass.hInstance = GetModuleHandleW(nullptr);
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.lpszClassName = L"RmwVideoTestWindow";
    RegisterClassExW(&windowClass);
    MONITORINFO monitor{};
    monitor.cbSize = sizeof(monitor);
    GetMonitorInfoW(MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY), &monitor);
    const RECT& r = monitor.rcMonitor;
    HWND window = fullscreen ? CreateWindowExW(WS_EX_TOPMOST, windowClass.lpszClassName, L"rmw fullscreen",
                                               WS_POPUP, r.left, r.top, r.right - r.left, r.bottom - r.top,
                                               nullptr, nullptr, windowClass.hInstance, nullptr)
                             : CreateWindowExW(0, windowClass.lpszClassName, L"rmw cover",
                                               WS_OVERLAPPEDWINDOW, r.left + 100, r.top + 100, 640, 480,
                                               nullptr, nullptr, windowClass.hInstance, nullptr);
    if (window == nullptr)
    {
        return 1;
    }
    ShowWindow(window, fullscreen ? SW_SHOW : SW_SHOWMAXIMIZED);
    SetForegroundWindow(window);
    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0)
    {
        DispatchMessageW(&message);
    }
    return 0;
}

} // namespace

int wmain(int argc, wchar_t** argv) // NOLINT(misc-use-internal-linkage): the entry point
{
    const std::wstring_view mode = argc > 1 ? argv[1] : L"";
    if (mode == L"encode" && argc > 2)
    {
        const UINT32 seconds = argc > 3 ? static_cast<UINT32>(std::wcstoul(argv[3], nullptr, 10)) : 10;
        if (FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED)) || FAILED(MFStartup(MF_VERSION)))
        {
            return 1;
        }
        const HRESULT hr = encode(argv[2], seconds == 0 ? 10 : seconds);
        MFShutdown();
        std::fwprintf(stdout, L"encode 0x%08X\n", static_cast<unsigned>(hr));
        return SUCCEEDED(hr) ? 0 : 1;
    }
    if (mode == L"fullscreen" || mode == L"cover")
    {
        return showWindow(mode == L"fullscreen");
    }
    std::fputs("usage: rmw_video_test_tool encode <file.mp4> [seconds] | fullscreen | cover\n", stderr);
    return 2;
}
