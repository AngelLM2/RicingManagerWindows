#include "Renderer.hpp"

#include "Composer.hpp"
#include "GraphicsParts.hpp"

#include "rmw/base/Log.hpp"
#include "rmw/base/Utf.hpp"
#include "rmw/platform/WinError.hpp"
#include "rmw/video/Playback.hpp"

#include <mferror.h>
#include <mfreadwrite.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <format>
#include <map>
#include <string>
#include <vector>

namespace rmw::wallpaper
{
namespace
{

using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;
using platform::hresultError;

constexpr DWORD kVideoStream = static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM);
// Frames the decoder may hold for us beyond its references: few, to keep the memory down
// (ADR-0006: the pipeline was most of the cost).
constexpr UINT32 kDecoderExtraFrames = 2;
constexpr std::uint32_t kDrawingFps = 60; // what we draw when the rice sets no limit

Result<void> check(HRESULT hr, std::string_view what)
{
    if (FAILED(hr))
    {
        return std::unexpected(hresultError(hr, what));
    }
    return {};
}

// What Windows lacks to play a video (docs/04 M6: "with an explanation of what is missing").
std::string missingDecoder(const GUID& codec)
{
    if (codec == MFVideoFormat_HEVC || codec == MFVideoFormat_HEVC_ES)
    {
        return "the video is HEVC (H.265): Windows plays it with the \"HEVC Video Extensions\" from the "
               "Microsoft Store; or save it as H.264";
    }
    if (codec == MFVideoFormat_AV1)
    {
        return "the video is AV1: Windows plays it with the \"AV1 Video Extension\" from the Microsoft "
               "Store; "
               "or save it as H.264";
    }
    if (codec == MFVideoFormat_VP90 || codec == MFVideoFormat_VP80)
    {
        return "the video is VP8/VP9: Windows plays it with the \"VP9 Video Extensions\" from the Microsoft "
               "Store; or save it as H.264";
    }
    return "Windows has no decoder for this video's format; H.264 always plays";
}

// COM on the renderer's thread (WIC decodes the pictures), for as long as it lives.
class ComThread
{
public:
    ComThread() noexcept
        : m_initialized(SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED)))
    {
    }
    ~ComThread()
    {
        if (m_initialized)
        {
            CoUninitialize();
        }
    }
    ComThread(const ComThread&) = delete;
    ComThread& operator=(const ComThread&) = delete;
    ComThread(ComThread&&) = delete;
    ComThread& operator=(ComThread&&) = delete;

private:
    bool m_initialized;
};

} // namespace

Graphics::Graphics()
    : m_parts(std::make_unique<Parts>())
{
}

Graphics::~Graphics() = default;

std::string Graphics::adapter() const
{
    return m_parts->adapter;
}

Result<std::shared_ptr<Graphics>> Graphics::create()
{
    std::shared_ptr<Graphics> graphics(new Graphics());
    Parts& p = *graphics->m_parts;
    const std::array levels = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1};
    if (auto created =
            check(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                                    D3D11_CREATE_DEVICE_VIDEO_SUPPORT | D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                                    levels.data(), static_cast<UINT>(levels.size()), D3D11_SDK_VERSION,
                                    p.device.put(), nullptr, p.context.put()),
                  "create a Direct3D 11 device with video support");
        !created)
    {
        return std::unexpected(std::move(created.error()));
    }
    // Every monitor's thread uses the one context.
    if (auto multithread = p.device.try_query<ID3D10Multithread>())
    {
        multithread->SetMultithreadProtected(TRUE);
    }
    p.videoDevice = p.device.try_query<ID3D11VideoDevice>();
    p.videoContext = p.context.try_query<ID3D11VideoContext>();
    if (!p.videoDevice || !p.videoContext)
    {
        return fail(ErrorCode::Unsupported, "this GPU has no video processor");
    }
    if (auto made = check(MFCreateDXGIDeviceManager(&p.resetToken, p.manager.put()),
                          "create the decoders' device manager");
        !made)
    {
        return std::unexpected(std::move(made.error()));
    }
    if (auto reset =
            check(p.manager->ResetDevice(p.device.get(), p.resetToken), "share the device with the decoders");
        !reset)
    {
        return std::unexpected(std::move(reset.error()));
    }
    auto dxgiDevice = p.device.try_query<IDXGIDevice>();
    wil::com_ptr_nothrow<IDXGIAdapter> adapter;
    if (!dxgiDevice || FAILED(dxgiDevice->GetAdapter(adapter.put())) ||
        FAILED(adapter->GetParent(IID_PPV_ARGS(p.factory.put()))))
    {
        return fail(ErrorCode::System, "cannot reach the GPU's DXGI factory");
    }
    DXGI_ADAPTER_DESC description{};
    adapter->GetDesc(&description);
    p.adapter = utf16To8Lossy(description.Description);
    if (auto composition =
            check(DCompositionCreateDevice(dxgiDevice.get(), IID_PPV_ARGS(p.composition.put())),
                  "create the DirectComposition device");
        !composition)
    {
        return std::unexpected(std::move(composition.error()));
    }
    // Direct2D on the same device: every renderer draws with its own context, on its own thread.
    const D2D1_FACTORY_OPTIONS options{};
    if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_MULTI_THREADED, __uuidof(ID2D1Factory1), &options,
                                 p.d2dFactory.put_void())) ||
        FAILED(p.d2dFactory->CreateDevice(dxgiDevice.get(), p.d2dDevice.put())))
    {
        log::warn("wallpaper: no Direct2D on {}: only plain videos play", p.adapter);
        p.d2dFactory.reset();
        p.d2dDevice.reset();
    }
    else
    {
        p.d2dLock = p.d2dFactory.try_query<ID2D1Multithread>();
    }
    return graphics;
}

struct Renderer::Pipeline
{
    wil::com_ptr_nothrow<IDXGISwapChain1> swapChain;
    wil::com_ptr_nothrow<IDCompositionTarget> target;
    wil::com_ptr_nothrow<IDCompositionVisual> visual;
    std::unique_ptr<Composer> composer; // effects and particles (M12); a picture or a color
    wil::com_ptr_nothrow<IMFSourceReader> reader;
    wil::com_ptr_nothrow<ID3D11VideoProcessorEnumerator> enumerator;
    wil::com_ptr_nothrow<ID3D11VideoProcessor> processor;
    std::map<std::pair<ID3D11Texture2D*, UINT>, wil::com_ptr_nothrow<ID3D11VideoProcessorInputView>> inputs;
    std::map<ID3D11Texture2D*, wil::com_ptr_nothrow<ID3D11VideoProcessorOutputView>> outputs;
    wil::com_ptr_nothrow<ID3D11Texture2D> upload; // software decoding: its frames go up through this
    std::vector<std::byte> rows;                  // a bottom-up frame, turned top-down
    bool hardware = true;                         // NV12 asked for (else RGB32)
    bool gpuFrames = true;                        // the frames come in GPU textures (DXVA)
    bool composed = true;                         // DirectComposition (else the window's own swap chain)
    int videoWidth = 0;
    int videoHeight = 0;
    double fps = 30.0;
    video::Placement placement;
};

Renderer::Renderer(std::shared_ptr<Graphics> graphics, void* window, RendererOptions options)
    : m_graphics(std::move(graphics))
    , m_window(window)
    , m_options(std::move(options))
    , m_pipeline(std::make_unique<Pipeline>())
{
}

Renderer::~Renderer()
{
    m_thread.request_stop();
    m_wake.notify_all();
    if (m_thread.joinable())
    {
        m_thread.join();
    }
}

std::string Renderer::describe() const
{
    const Pipeline& p = *m_pipeline;
    if (m_options.source != video::Source::Video)
    {
        return std::format("{}, into {}x{}{}", p.composer ? p.composer->describe() : std::string("nothing"),
                           m_options.width, m_options.height, p.composed ? "" : " (window swap chain)");
    }
    return std::format("{}x{} at {:.0f} fps{}, {} decoding, into {}x{}{}{}", p.videoWidth, p.videoHeight, p.fps,
                       m_options.effects.speed != 1.0 ? std::format(" (speed {})", m_options.effects.speed) : "",
                       p.gpuFrames ? "hardware" : "software", m_options.width, m_options.height,
                       p.composed ? "" : " (window swap chain)",
                       p.composer ? std::format(" ({})", p.composer->describe()) : "");
}

std::string Renderer::failure() const
{
    const std::scoped_lock lock(m_mutex);
    return m_failure;
}

void Renderer::start()
{
    m_thread = std::jthread([this](const std::stop_token& stop) { run(stop); });
}

Result<void> Renderer::build()
{
    const Graphics::Parts& g = m_graphics->parts();
    Pipeline& p = *m_pipeline;

    // The surface: DirectComposition shows it in the window (ADR-0006); a window it cannot target
    // gets a flip swap chain of its own (docs/04 M6: the HWND way as the fallback).
    DXGI_SWAP_CHAIN_DESC1 chain{};
    chain.Width = static_cast<UINT>(m_options.width);
    chain.Height = static_cast<UINT>(m_options.height);
    chain.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    chain.SampleDesc = {1, 0};
    chain.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    chain.BufferCount = 2;
    chain.Scaling = DXGI_SCALING_STRETCH;
    chain.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
    chain.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
    const auto compose = [&]() -> Result<void> {
        const std::scoped_lock lock(g.compositionLock); // one device, a renderer thread per monitor
        if (auto made = check(
                g.factory->CreateSwapChainForComposition(g.device.get(), &chain, nullptr, p.swapChain.put()),
                "create the swap chain");
            !made)
        {
            return made;
        }
        if (auto shown =
                check(g.composition->CreateTargetForHwnd(static_cast<HWND>(m_window), TRUE, p.target.put()),
                      "show the swap chain in the window");
            !shown)
        {
            return shown;
        }
        g.composition->CreateVisual(p.visual.put());
        p.visual->SetContent(p.swapChain.get());
        p.target->SetRoot(p.visual.get());
        return check(g.composition->Commit(), "commit the composition");
    };
    if (auto composed = compose(); !composed)
    {
        log::warn("wallpaper: DirectComposition: {}; presenting to the window directly",
                  composed.error().toString());
        p.target.reset();
        p.visual.reset();
        p.swapChain.reset();
        chain.AlphaMode = DXGI_ALPHA_MODE_UNSPECIFIED;
        if (auto made = check(g.factory->CreateSwapChainForHwnd(g.device.get(), static_cast<HWND>(m_window),
                                                                &chain, nullptr, nullptr, p.swapChain.put()),
                              "create the window's swap chain");
            !made)
        {
            return made;
        }
        p.composed = false;
    }

    if (m_options.composed())
    {
        p.composer = std::make_unique<Composer>(m_graphics, m_options);
        const D2DLock lock(g);
        if (auto built = p.composer->build(); !built)
        {
            return built;
        }
    }
    return m_options.source == video::Source::Video ? buildVideo() : Result<void>{};
}

Result<void> Renderer::buildVideo()
{
    const Graphics::Parts& g = m_graphics->parts();
    Pipeline& p = *m_pipeline;

    // The decoder: DXVA when the GPU can, the device shared with it. The plain "video processing"
    // attribute refuses a D3D manager (E_INVALIDARG); the advanced one converts on the GPU.
    wil::com_ptr_nothrow<IMFAttributes> attributes;
    MFCreateAttributes(attributes.put(), 3);
    attributes->SetUnknown(MF_SOURCE_READER_D3D_MANAGER, g.manager.get());
    attributes->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE);
    attributes->SetUINT32(MF_SOURCE_READER_ENABLE_ADVANCED_VIDEO_PROCESSING, TRUE);
    if (auto opened =
            check(MFCreateSourceReaderFromURL(m_options.file.c_str(), attributes.get(), p.reader.put()),
                  "open the video");
        !opened)
    {
        return opened;
    }
    p.reader->SetStreamSelection(static_cast<DWORD>(MF_SOURCE_READER_ALL_STREAMS), FALSE);
    p.reader->SetStreamSelection(kVideoStream, TRUE);
    GUID codec{};
    {
        wil::com_ptr_nothrow<IMFMediaType> native;
        if (auto read =
                check(p.reader->GetNativeMediaType(kVideoStream, 0, native.put()), "read the video's format");
            !read)
        {
            return read;
        }
        native->GetGUID(MF_MT_SUBTYPE, &codec);
        UINT32 width = 0;
        UINT32 height = 0;
        MFGetAttributeSize(native.get(), MF_MT_FRAME_SIZE, &width, &height);
        UINT32 numerator = 0;
        UINT32 denominator = 0;
        if (SUCCEEDED(MFGetAttributeRatio(native.get(), MF_MT_FRAME_RATE, &numerator, &denominator)) &&
            denominator != 0 && numerator != 0)
        {
            p.fps = static_cast<double>(numerator) / denominator;
        }
        p.videoWidth = static_cast<int>(width);
        p.videoHeight = static_cast<int>(height);
    }
    if (p.videoWidth <= 0 || p.videoHeight <= 0 || p.videoWidth > 8192 || p.videoHeight > 8192)
    {
        return fail(ErrorCode::InvalidData, "the video has no usable frame size");
    }
    const auto useFormat = [&](const GUID& subtype) {
        wil::com_ptr_nothrow<IMFMediaType> type;
        MFCreateMediaType(type.put());
        type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        type->SetGUID(MF_MT_SUBTYPE, subtype);
        return p.reader->SetCurrentMediaType(kVideoStream, nullptr, type.get());
    };
    if (FAILED(useFormat(MFVideoFormat_NV12)))
    {
        if (FAILED(useFormat(MFVideoFormat_RGB32)))
        {
            return fail(ErrorCode::Unsupported, missingDecoder(codec));
        }
        p.hardware = false;
    }
    // Few frames in the decoder's hands.
    if (auto readerEx = p.reader.try_query<IMFSourceReaderEx>())
    {
        wil::com_ptr_nothrow<IMFTransform> decoder;
        GUID category{};
        if (SUCCEEDED(readerEx->GetTransformForStream(kVideoStream, 0, &category, decoder.put())) && decoder)
        {
            wil::com_ptr_nothrow<IMFAttributes> decoderAttributes;
            if (SUCCEEDED(decoder->GetAttributes(decoderAttributes.put())) && decoderAttributes)
            {
                decoderAttributes->SetUINT32(MF_SA_MINIMUM_OUTPUT_SAMPLE_COUNT, kDecoderExtraFrames);
            }
        }
    }
    // A decoder without DXVA hands frames in memory: those come as RGB and go up through a texture
    // (each frame is looked at: which way it came decides how it goes up).
    {
        DWORD stream = 0;
        DWORD flags = 0;
        LONGLONG time = 0;
        wil::com_ptr_nothrow<IMFSample> first;
        if (auto decoded = check(p.reader->ReadSample(kVideoStream, 0, &stream, &flags, &time, first.put()),
                                 "decode the first frame");
            !decoded)
        {
            return decoded;
        }
        wil::com_ptr_nothrow<IMFMediaBuffer> buffer;
        if (!first || FAILED(first->GetBufferByIndex(0, buffer.put())))
        {
            return fail(ErrorCode::InvalidData, "the video has no frames");
        }
        if (!buffer.try_query<IMFDXGIBuffer>())
        {
            if (p.hardware)
            {
                if (auto rgb = check(useFormat(MFVideoFormat_RGB32), "decode the video in software"); !rgb)
                {
                    return rgb;
                }
            }
            p.hardware = false;
            p.gpuFrames = false;
        }
        PROPVARIANT start{};
        start.vt = VT_I8;
        start.hVal.QuadPart = 0;
        p.reader->SetCurrentPosition(GUID_NULL, start);
    }

    // The video processor: scales (and converts NV12) straight into the swap chain, or into the
    // Composer's frames (a little larger than the monitor when the picture moves).
    const int outputWidth = p.composer ? p.composer->frameWidth() : m_options.width;
    const int outputHeight = p.composer ? p.composer->frameHeight() : m_options.height;
    D3D11_VIDEO_PROCESSOR_CONTENT_DESC content{};
    content.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
    content.InputFrameRate = {static_cast<UINT>(std::lround(p.fps)), 1};
    content.InputWidth = static_cast<UINT>(p.videoWidth);
    content.InputHeight = static_cast<UINT>(p.videoHeight);
    content.OutputFrameRate = content.InputFrameRate;
    content.OutputWidth = static_cast<UINT>(outputWidth);
    content.OutputHeight = static_cast<UINT>(outputHeight);
    content.Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;
    if (auto made = check(g.videoDevice->CreateVideoProcessorEnumerator(&content, p.enumerator.put()),
                          "describe the video processing");
        !made)
    {
        return made;
    }
    if (auto made = check(g.videoDevice->CreateVideoProcessor(p.enumerator.get(), 0, p.processor.put()),
                          "create the video processor");
        !made)
    {
        return made;
    }
    if (p.composer)
    {
        const D2DLock lock(g);
        if (auto frames = p.composer->frames(); !frames)
        {
            return std::unexpected(std::move(frames.error()));
        }
    }
    p.placement = video::place(m_options.fit, p.videoWidth, p.videoHeight, outputWidth, outputHeight);
    const RECT source{p.placement.source.left, p.placement.source.top, p.placement.source.right,
                      p.placement.source.bottom};
    const RECT target{p.placement.target.left, p.placement.target.top, p.placement.target.right,
                      p.placement.target.bottom};
    const RECT whole{0, 0, outputWidth, outputHeight};
    g.videoContext->VideoProcessorSetStreamFrameFormat(p.processor.get(), 0,
                                                       D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE);
    g.videoContext->VideoProcessorSetStreamAutoProcessingMode(p.processor.get(), 0, FALSE);
    g.videoContext->VideoProcessorSetStreamSourceRect(p.processor.get(), 0, TRUE, &source);
    g.videoContext->VideoProcessorSetStreamDestRect(p.processor.get(), 0, TRUE, &target);
    g.videoContext->VideoProcessorSetOutputTargetRect(p.processor.get(), TRUE, &whole);
    const auto& background = m_options.background;
    const D3D11_VIDEO_COLOR around{.RGBA = {static_cast<float>(background.r) / 255.0F,
                                            static_cast<float>(background.g) / 255.0F,
                                            static_cast<float>(background.b) / 255.0F, 1.0F}};
    g.videoContext->VideoProcessorSetOutputBackgroundColor(p.processor.get(), FALSE, &around);
    return {};
}

void Renderer::setPaused(bool paused)
{
    {
        const std::scoped_lock lock(m_mutex);
        m_paused = paused;
    }
    m_wake.notify_all();
}

void Renderer::lose(long hr, std::string_view what)
{
    if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET || hr == D2DERR_RECREATE_TARGET ||
        hr == MF_E_DXGI_DEVICE_NOT_INITIALIZED || hr == MF_E_VIDEO_RECORDING_DEVICE_INVALIDATED)
    {
        m_deviceLost = true;
    }
    const auto error = hresultError(hr, what).toString();
    log::warn("wallpaper: {}: {}", what, error);
    {
        const std::scoped_lock lock(m_mutex);
        m_failure = error;
    }
    m_failed = true;
}

void Renderer::run(const std::stop_token& stop)
{
    const ComThread com;
    const Graphics::Parts& g = m_graphics->parts();
    if (auto built = build(); !built)
    {
        {
            const std::scoped_lock lock(m_mutex);
            m_failure = built.error().toString();
        }
        m_deviceLost = FAILED(g.device->GetDeviceRemovedReason());
        m_failed = true;
        return;
    }
    m_ready = true;
    if (m_options.source == video::Source::Video)
    {
        runVideo(stop);
    }
    else
    {
        runDrawing(stop);
    }
    m_pipeline->composer.reset(); // on this thread, before COM goes
}

void Renderer::runVideo(const std::stop_token& stop)
{
    const Graphics::Parts& g = m_graphics->parts();
    Pipeline& p = *m_pipeline;
    video::FramePacer pacer(m_options.fpsLimit);
    Clock::time_point origin = Clock::now(); // when the frame stamped `base` is due
    LONGLONG base = -1;
    wil::com_ptr_nothrow<IMFSample> pending;
    LONGLONG pendingTime = 0;
    // Faster or slower than it was made: its timestamps, stretched.
    const double speed = std::clamp(m_options.effects.speed, 0.25, 4.0);
    Clock::time_point lastShown = Clock::now();

    // One frame: into the swap chain's back buffer (through the Composer when it has effects or
    // particles), then shown at the next vertical blank.
    const auto present = [&](IMFSample& sample) -> HRESULT {
        wil::com_ptr_nothrow<IMFMediaBuffer> buffer;
        RETURN_IF_FAILED(sample.GetBufferByIndex(0, buffer.put()));
        const D2DLock lock(g);
        ID3D11VideoProcessorInputView* input = nullptr;
        if (auto dxgi = buffer.try_query<IMFDXGIBuffer>())
        {
            wil::com_ptr_nothrow<ID3D11Texture2D> texture;
            UINT index = 0;
            if (FAILED(dxgi->GetResource(IID_PPV_ARGS(texture.put()))) ||
                FAILED(dxgi->GetSubresourceIndex(&index)))
            {
                return E_UNEXPECTED;
            }
            auto& view = p.inputs[{texture.get(), index}];
            if (!view)
            {
                D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC desc{};
                desc.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
                desc.Texture2D.ArraySlice = index;
                RETURN_IF_FAILED(g.videoDevice->CreateVideoProcessorInputView(
                    texture.get(), p.enumerator.get(), &desc, view.put()));
            }
            input = view.get();
        }
        else
        {
            if (p.hardware)
            {
                return E_UNEXPECTED; // NV12 in memory: not a way we take
            }
            if (!p.upload)
            {
                D3D11_TEXTURE2D_DESC upload{};
                upload.Width = static_cast<UINT>(p.videoWidth);
                upload.Height = static_cast<UINT>(p.videoHeight);
                upload.MipLevels = 1;
                upload.ArraySize = 1;
                upload.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
                upload.SampleDesc = {1, 0};
                upload.Usage = D3D11_USAGE_DEFAULT;
                upload.BindFlags = D3D11_BIND_SHADER_RESOURCE;
                RETURN_IF_FAILED(g.device->CreateTexture2D(&upload, nullptr, p.upload.put()));
            }
            auto image = buffer.try_query<IMF2DBuffer>();
            BYTE* scanline = nullptr;
            LONG pitch = 0;
            if (!image || FAILED(image->Lock2D(&scanline, &pitch)))
            {
                return E_UNEXPECTED;
            }
            const auto rowBytes = static_cast<std::size_t>(p.videoWidth) * 4;
            const BYTE* rows = scanline;
            UINT rowPitch = static_cast<UINT>(pitch);
            if (pitch < 0) // bottom-up: top-down for the texture
            {
                p.rows.resize(rowBytes * static_cast<std::size_t>(p.videoHeight));
                for (int y = 0; y < p.videoHeight; ++y)
                {
                    std::memcpy(p.rows.data() + (rowBytes * static_cast<std::size_t>(y)),
                                scanline + (static_cast<std::ptrdiff_t>(pitch) * y), rowBytes);
                }
                rows = reinterpret_cast<const BYTE*>(p.rows.data());
                rowPitch = static_cast<UINT>(rowBytes);
            }
            g.context->UpdateSubresource(p.upload.get(), 0, nullptr, rows, rowPitch, 0);
            image->Unlock2D();
            auto& view = p.inputs[{p.upload.get(), 0}];
            if (!view)
            {
                D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC desc{};
                desc.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
                RETURN_IF_FAILED(g.videoDevice->CreateVideoProcessorInputView(
                    p.upload.get(), p.enumerator.get(), &desc, view.put()));
            }
            input = view.get();
        }
        wil::com_ptr_nothrow<ID3D11Texture2D> back;
        RETURN_IF_FAILED(p.swapChain->GetBuffer(0, IID_PPV_ARGS(back.put())));
        ID3D11Texture2D* output = back.get();
        if (p.composer)
        {
            auto frames = p.composer->frames();
            if (!frames)
            {
                return E_UNEXPECTED;
            }
            output = *frames;
        }
        auto& view = p.outputs[output];
        if (!view)
        {
            D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC desc{};
            desc.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
            RETURN_IF_FAILED(
                g.videoDevice->CreateVideoProcessorOutputView(output, p.enumerator.get(), &desc, view.put()));
        }
        D3D11_VIDEO_PROCESSOR_STREAM stream{};
        stream.Enable = TRUE;
        stream.pInputSurface = input;
        RETURN_IF_FAILED(g.videoContext->VideoProcessorBlt(p.processor.get(), view.get(), 0, 1, &stream));
        if (p.composer)
        {
            const auto now = Clock::now();
            p.composer->advance(std::chrono::duration<double>(now - lastShown).count());
            lastShown = now;
            RETURN_IF_FAILED(p.composer->draw(back.get()));
        }
        return S_OK;
    };

    while (!stop.stop_requested())
    {
        {
            std::unique_lock lock(m_mutex);
            if (m_paused)
            {
                const auto pausedAt = Clock::now();
                m_wake.wait(lock, stop, [this] { return !m_paused; });
                origin += Clock::now() - pausedAt; // on from where it stopped
                lastShown = Clock::now();
                pacer.reset();
                continue;
            }
        }
        if (!pending)
        {
            DWORD stream = 0;
            DWORD flags = 0;
            LONGLONG time = 0;
            const HRESULT hr = p.reader->ReadSample(kVideoStream, 0, &stream, &flags, &time, pending.put());
            if (FAILED(hr))
            {
                lose(hr, "decode a frame");
                return;
            }
            if ((flags & MF_SOURCE_READERF_ENDOFSTREAM) != 0)
            {
                // A loop: from the start, timed anew.
                PROPVARIANT start{};
                start.vt = VT_I8;
                start.hVal.QuadPart = 0;
                p.reader->SetCurrentPosition(GUID_NULL, start);
                pending.reset();
                base = -1;
                continue;
            }
            if (!pending)
            {
                continue; // a gap in the stream
            }
            pendingTime = time;
            if (base < 0)
            {
                base = time;
                origin = Clock::now();
            }
        }
        const auto due = origin + std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(
                                      static_cast<double>(pendingTime - base) * 100e-9 / speed));
        const auto now = Clock::now();
        if (now < due)
        {
            std::unique_lock lock(m_mutex);
            m_wake.wait_until(lock, stop, std::min(due, now + 100ms), [this] { return m_paused; });
            continue;
        }
        if (pacer.due(now))
        {
            HRESULT hr = present(*pending);
            if (SUCCEEDED(hr))
            {
                hr = p.swapChain->Present(1, 0);
            }
            if (FAILED(hr))
            {
                lose(hr, "show a frame");
                return;
            }
            ++m_presented;
        }
        pending.reset();
    }
}

void Renderer::runDrawing(const std::stop_token& stop)
{
    const Graphics::Parts& g = m_graphics->parts();
    const Pipeline& p = *m_pipeline;
    const std::uint32_t fps = m_options.fpsLimit == 0 ? kDrawingFps : m_options.fpsLimit;
    const auto period = std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(1.0 / fps));
    auto last = Clock::now();
    auto next = last;
    bool drawn = false;
    while (!stop.stop_requested())
    {
        {
            std::unique_lock lock(m_mutex);
            if (m_paused)
            {
                m_wake.wait(lock, stop, [this] { return !m_paused; });
                last = Clock::now(); // nothing jumps after a pause
                next = last;
                continue;
            }
            if (drawn && !p.composer->animated())
            {
                // A still picture: drawn once, nothing to do until the end.
                m_wake.wait(lock, stop, [] { return false; });
                continue;
            }
        }
        const auto now = Clock::now();
        if (now < next)
        {
            std::unique_lock lock(m_mutex);
            m_wake.wait_until(lock, stop, next, [this] { return m_paused; });
            continue;
        }
        next = std::max(next + period, now);
        p.composer->advance(std::chrono::duration<double>(now - last).count());
        last = now;
        if (drawn && p.composer->settled())
        {
            continue; // the same frame again: not shown
        }
        HRESULT hr = S_OK;
        {
            const D2DLock lock(g);
            wil::com_ptr_nothrow<ID3D11Texture2D> back;
            hr = p.swapChain->GetBuffer(0, IID_PPV_ARGS(back.put()));
            if (SUCCEEDED(hr))
            {
                hr = p.composer->draw(back.get());
            }
        }
        if (SUCCEEDED(hr))
        {
            hr = p.swapChain->Present(1, 0);
        }
        if (FAILED(hr))
        {
            lose(hr, "draw the wallpaper");
            return;
        }
        drawn = true;
        ++m_presented;
    }
}

} // namespace rmw::wallpaper
