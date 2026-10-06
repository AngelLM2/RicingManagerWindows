// rmw-rice-art — the official rices' pictures, video loops and cursors (docs/11), drawn from code:
// original art, nothing downloaded and nothing made by AI. Direct2D (with its blur, for the glows)
// and DirectWrite draw, WIC writes the PNGs, Media Foundation encodes the H.264 loops (Windows' own
// encoder), and the cursors are written as .cur and .ani by hand. Run by the `rices` build target:
//
//   rmw-rice-art <rice> <folder> [--quick]
//
// writes <folder>\assets\... for one of: diva-stage, tokyo-midnight, sakura-dawn, mocha-kawaii,
// your-rice. --quick: small pictures and no videos (for a look while drawing).

#include <windows.h>

#include <d2d1_1.h>
#include <d2d1effects.h>
#include <codecapi.h>
#include <d3d11.h>
#include <dwrite.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <wincodec.h>
#include <wil/com.h>
#include <wil/resource.h>
#include <wil/result.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <numbers>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace
{

namespace fs = std::filesystem;
using wil::com_ptr;

constexpr float kPi = std::numbers::pi_v<float>;
constexpr float kTau = 2.0F * kPi;

// ---- colors and randomness --------------------------------------------------------------------------

struct Color
{
    float r = 0;
    float g = 0;
    float b = 0;
    float a = 1;
};

constexpr Color hex(std::uint32_t rgb, float alpha = 1.0F)
{
    return {static_cast<float>((rgb >> 16U) & 0xFFU) / 255.0F, static_cast<float>((rgb >> 8U) & 0xFFU) / 255.0F,
            static_cast<float>(rgb & 0xFFU) / 255.0F, alpha};
}

Color withAlpha(Color c, float alpha)
{
    c.a = alpha;
    return c;
}

Color mix(Color a, Color b, float t)
{
    return {a.r + ((b.r - a.r) * t), a.g + ((b.g - a.g) * t), a.b + ((b.b - a.b) * t), a.a + ((b.a - a.a) * t)};
}

// A gradient stop: where (0..1) and which color.
struct Stop
{
    float at;
    Color color;
};

D2D1_COLOR_F d2d(Color c)
{
    return {c.r, c.g, c.b, c.a};
}

// The same seed draws the same picture: builds are reproducible.
class Random
{
public:
    explicit Random(std::uint64_t seed)
        : m_state(seed * 0x9E3779B97F4A7C15ULL + 1)
    {
    }
    float next() // [0, 1)
    {
        m_state ^= m_state << 13U;
        m_state ^= m_state >> 7U;
        m_state ^= m_state << 17U;
        return static_cast<float>(m_state >> 40U) / static_cast<float>(1ULL << 24U);
    }
    float range(float low, float high) { return low + ((high - low) * next()); }
    template <class T>
    const T& pick(std::span<const T> items)
    {
        return items[std::min(items.size() - 1, static_cast<std::size_t>(next() * static_cast<float>(items.size())))];
    }

private:
    std::uint64_t m_state;
};

float fract(float x)
{
    return x - std::floor(x);
}

// ---- drawing ----------------------------------------------------------------------------------------

// Direct2D on a Direct3D device (the GPU, or WARP when there is none): its effects (the blur of
// the glows) need a device context.
struct Gpu
{
    com_ptr<ID3D11Device> d3d;
    com_ptr<ID2D1Factory1> factory;
    com_ptr<ID2D1Device> device;
    com_ptr<ID2D1DeviceContext> dc;
    com_ptr<IDWriteFactory> write;
    com_ptr<IWICImagingFactory> wic;
    com_ptr<ID2D1Effect> blur;
    com_ptr<ID2D1Effect> fade;

    Gpu()
    {
        const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0};
        HRESULT hr = E_FAIL;
        for (const auto driver : {D3D_DRIVER_TYPE_HARDWARE, D3D_DRIVER_TYPE_WARP})
        {
            hr = D3D11CreateDevice(nullptr, driver, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels,
                                   static_cast<UINT>(std::size(levels)), D3D11_SDK_VERSION, d3d.put(), nullptr,
                                   nullptr);
            if (SUCCEEDED(hr))
            {
                break;
            }
        }
        THROW_IF_FAILED(hr);
        THROW_IF_FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, factory.put()));
        THROW_IF_FAILED(factory->CreateDevice(d3d.query<IDXGIDevice>().get(), device.put()));
        THROW_IF_FAILED(device->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, dc.put()));
        THROW_IF_FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                            reinterpret_cast<IUnknown**>(write.put())));
        THROW_IF_FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(wic.put())));
        THROW_IF_FAILED(dc->CreateEffect(CLSID_D2D1GaussianBlur, blur.put()));
        THROW_IF_FAILED(dc->CreateEffect(CLSID_D2D1ColorMatrix, fade.put()));
        fade->SetInputEffect(0, blur.get());
        THROW_IF_FAILED(blur->SetValue(D2D1_GAUSSIANBLUR_PROP_BORDER_MODE, D2D1_BORDER_MODE_HARD));
        THROW_IF_FAILED(blur->SetValue(D2D1_GAUSSIANBLUR_PROP_OPTIMIZATION, D2D1_GAUSSIANBLUR_OPTIMIZATION_QUALITY));
    }
};

// What one picture is drawn on: a target, a layer for the glows, a copy the CPU reads.
class Surface
{
public:
    Surface(Gpu& gpu, UINT width, UINT height)
        : m_gpu(gpu)
        , m_width(width)
        , m_height(height)
    {
        const D2D1_SIZE_U size{width, height};
        const D2D1_PIXEL_FORMAT format{DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED};
        const D2D1_BITMAP_PROPERTIES1 target{format, 96, 96, D2D1_BITMAP_OPTIONS_TARGET, nullptr};
        THROW_IF_FAILED(gpu.dc->CreateBitmap(size, nullptr, 0, target, m_target.put()));
        THROW_IF_FAILED(gpu.dc->CreateBitmap(size, nullptr, 0, target, m_layer.put()));
        const D2D1_BITMAP_PROPERTIES1 readback{format, 96, 96,
                                               D2D1_BITMAP_OPTIONS_CPU_READ | D2D1_BITMAP_OPTIONS_CANNOT_DRAW, nullptr};
        THROW_IF_FAILED(gpu.dc->CreateBitmap(size, nullptr, 0, readback, m_readback.put()));
    }

    ID2D1DeviceContext* dc() const { return m_gpu.dc.get(); }
    Gpu& gpu() const { return m_gpu; }
    float width() const { return static_cast<float>(m_width); }
    float height() const { return static_cast<float>(m_height); }

    void begin(Color clear)
    {
        dc()->SetTarget(m_target.get());
        dc()->BeginDraw();
        dc()->SetTransform(D2D1::Matrix3x2F::Identity());
        dc()->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        dc()->Clear(d2d(clear));
    }

    void end() { THROW_IF_FAILED(dc()->EndDraw()); }

    // `draw` on a layer of its own, blurred and added to the picture (light adds up), then the
    // layer itself on top when `sharp`.
    void glow(float radius, float strength, const std::function<void()>& draw, bool sharp = true)
    {
        const auto transform = [this] {
            D2D1_MATRIX_3X2_F m;
            dc()->GetTransform(&m);
            return m;
        }();
        dc()->SetTarget(m_layer.get());
        dc()->Clear(D2D1::ColorF(0, 0, 0, 0));
        draw();
        dc()->SetTarget(m_target.get());
        dc()->SetTransform(D2D1::Matrix3x2F::Identity());
        m_gpu.blur->SetInput(0, m_layer.get());
        THROW_IF_FAILED(m_gpu.blur->SetValue(D2D1_GAUSSIANBLUR_PROP_STANDARD_DEVIATION, radius));
        const D2D1_MATRIX_5X4_F scale = D2D1::Matrix5x4F(1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, strength, 0, 0, 0, 0);
        THROW_IF_FAILED(m_gpu.fade->SetValue(D2D1_COLORMATRIX_PROP_COLOR_MATRIX, scale));
        THROW_IF_FAILED(m_gpu.fade->SetValue(D2D1_COLORMATRIX_PROP_CLAMP_OUTPUT, TRUE));
        dc()->DrawImage(m_gpu.fade.get(), D2D1_INTERPOLATION_MODE_LINEAR, D2D1_COMPOSITE_MODE_PLUS);
        if (sharp)
        {
            dc()->DrawImage(m_layer.get());
        }
        dc()->SetTransform(transform);
    }

    // The pixels drawn (premultiplied BGRA, top-down, `width * 4` bytes a row).
    std::vector<std::uint8_t> pixels()
    {
        THROW_IF_FAILED(m_readback->CopyFromBitmap(nullptr, m_target.get(), nullptr));
        D2D1_MAPPED_RECT mapped{};
        THROW_IF_FAILED(m_readback->Map(D2D1_MAP_OPTIONS_READ, &mapped));
        std::vector<std::uint8_t> out(static_cast<std::size_t>(m_width) * m_height * 4);
        for (UINT y = 0; y < m_height; ++y)
        {
            std::copy_n(mapped.bits + (static_cast<std::size_t>(y) * mapped.pitch), static_cast<std::size_t>(m_width) * 4,
                        out.data() + (static_cast<std::size_t>(y) * m_width * 4));
        }
        THROW_IF_FAILED(m_readback->Unmap());
        return out;
    }

private:
    Gpu& m_gpu;
    UINT m_width;
    UINT m_height;
    com_ptr<ID2D1Bitmap1> m_target;
    com_ptr<ID2D1Bitmap1> m_layer;
    com_ptr<ID2D1Bitmap1> m_readback;
};

com_ptr<ID2D1SolidColorBrush> solid(ID2D1DeviceContext* dc, Color c)
{
    com_ptr<ID2D1SolidColorBrush> brush;
    THROW_IF_FAILED(dc->CreateSolidColorBrush(d2d(c), brush.put()));
    return brush;
}

com_ptr<ID2D1GradientStopCollection> stops(ID2D1DeviceContext* dc, std::initializer_list<Stop> list)
{
    std::vector<D2D1_GRADIENT_STOP> items;
    items.reserve(list.size());
    for (const auto& stop : list)
    {
        items.push_back({stop.at, d2d(stop.color)});
    }
    com_ptr<ID2D1GradientStopCollection> collection;
    THROW_IF_FAILED(dc->CreateGradientStopCollection(items.data(), static_cast<UINT32>(items.size()), collection.put()));
    return collection;
}

com_ptr<ID2D1LinearGradientBrush> linear(ID2D1DeviceContext* dc, D2D1_POINT_2F from, D2D1_POINT_2F to,
                                         std::initializer_list<Stop> list)
{
    com_ptr<ID2D1LinearGradientBrush> brush;
    THROW_IF_FAILED(dc->CreateLinearGradientBrush({from, to}, stops(dc, list).get(), brush.put()));
    return brush;
}

com_ptr<ID2D1RadialGradientBrush> radial(ID2D1DeviceContext* dc, D2D1_POINT_2F center, float rx, float ry,
                                         std::initializer_list<Stop> list)
{
    com_ptr<ID2D1RadialGradientBrush> brush;
    THROW_IF_FAILED(dc->CreateRadialGradientBrush({center, {0, 0}, rx, ry}, stops(dc, list).get(), brush.put()));
    return brush;
}

// A soft round light: `color` fading out to nothing.
void blob(ID2D1DeviceContext* dc, D2D1_POINT_2F center, float rx, float ry, Color color)
{
    dc->FillEllipse({center, rx, ry},
                    radial(dc, center, rx, ry, {{0, color}, {0.45F, withAlpha(color, color.a * 0.45F)}, {1, withAlpha(color, 0)}})
                        .get());
}

com_ptr<ID2D1PathGeometry> path(ID2D1DeviceContext* dc, const std::function<void(ID2D1GeometrySink*)>& build)
{
    com_ptr<ID2D1Factory> factory;
    dc->GetFactory(factory.put());
    com_ptr<ID2D1PathGeometry> geometry;
    THROW_IF_FAILED(factory->CreatePathGeometry(geometry.put()));
    com_ptr<ID2D1GeometrySink> sink;
    THROW_IF_FAILED(geometry->Open(sink.put()));
    build(sink.get());
    THROW_IF_FAILED(sink->Close());
    return geometry;
}

com_ptr<ID2D1PathGeometry> polygon(ID2D1DeviceContext* dc, std::span<const D2D1_POINT_2F> points)
{
    return path(dc, [&](ID2D1GeometrySink* sink) {
        sink->BeginFigure(points[0], D2D1_FIGURE_BEGIN_FILLED);
        for (std::size_t i = 1; i < points.size(); ++i)
        {
            sink->AddLine(points[i]);
        }
        sink->EndFigure(D2D1_FIGURE_END_CLOSED);
    });
}

void text(Surface& s, std::wstring_view what, const wchar_t* font, float size, D2D1_POINT_2F center, Color color,
          DWRITE_FONT_WEIGHT weight = DWRITE_FONT_WEIGHT_BOLD, float angle = 0)
{
    com_ptr<IDWriteTextFormat> format;
    THROW_IF_FAILED(s.gpu().write->CreateTextFormat(font, nullptr, weight, DWRITE_FONT_STYLE_NORMAL,
                                                     DWRITE_FONT_STRETCH_NORMAL, size, L"ja-jp", format.put()));
    format->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    format->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    D2D1_MATRIX_3X2_F before;
    s.dc()->GetTransform(&before);
    s.dc()->SetTransform(D2D1::Matrix3x2F::Rotation(angle, center) * before);
    const float box = size * static_cast<float>(what.size() + 2);
    s.dc()->DrawTextW(what.data(), static_cast<UINT32>(what.size()), format.get(),
                      {center.x - box, center.y - size, center.x + box, center.y + size}, solid(s.dc(), color).get(),
                      D2D1_DRAW_TEXT_OPTIONS_ENABLE_COLOR_FONT);
    s.dc()->SetTransform(before);
}

// ---- files ------------------------------------------------------------------------------------------

void savePng(Surface& surface, const fs::path& file)
{
    auto pixels = surface.pixels();
    const auto width = static_cast<UINT>(surface.width());
    const auto height = static_cast<UINT>(surface.height());
    auto& wic = surface.gpu().wic;
    com_ptr<IWICStream> stream;
    THROW_IF_FAILED(wic->CreateStream(stream.put()));
    THROW_IF_FAILED(stream->InitializeFromFilename(file.c_str(), GENERIC_WRITE));
    com_ptr<IWICBitmapEncoder> encoder;
    THROW_IF_FAILED(wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, encoder.put()));
    THROW_IF_FAILED(encoder->Initialize(stream.get(), WICBitmapEncoderNoCache));
    com_ptr<IWICBitmapFrameEncode> frame;
    THROW_IF_FAILED(encoder->CreateNewFrame(frame.put(), nullptr));
    THROW_IF_FAILED(frame->Initialize(nullptr));
    THROW_IF_FAILED(frame->SetSize(width, height));
    // Opaque pictures: no alpha in the file (smaller, and nothing to premultiply).
    WICPixelFormatGUID format = GUID_WICPixelFormat24bppBGR;
    THROW_IF_FAILED(frame->SetPixelFormat(&format));
    std::vector<std::uint8_t> rgb(static_cast<std::size_t>(width) * height * 3);
    for (std::size_t i = 0, j = 0; i < pixels.size(); i += 4, j += 3)
    {
        rgb[j] = pixels[i];
        rgb[j + 1] = pixels[i + 1];
        rgb[j + 2] = pixels[i + 2];
    }
    THROW_IF_FAILED(frame->WritePixels(height, width * 3, static_cast<UINT>(rgb.size()), rgb.data()));
    THROW_IF_FAILED(frame->Commit());
    THROW_IF_FAILED(encoder->Commit());
    std::wprintf(L"  %ls (%ux%u)\n", file.filename().c_str(), width, height);
}

// A seamless loop: frame f draws phase f / frames, so the last frame leads back to the first.
void saveLoop(Surface& surface, const fs::path& file, UINT fps, UINT seconds, const std::function<void(float)>& draw)
{
    const auto width = static_cast<UINT>(surface.width());
    const auto height = static_cast<UINT>(surface.height());
    com_ptr<IMFAttributes> attributes;
    THROW_IF_FAILED(MFCreateAttributes(attributes.put(), 1));
    THROW_IF_FAILED(attributes->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE));
    std::error_code ec;
    fs::remove(file, ec);
    com_ptr<IMFSinkWriter> writer;
    THROW_IF_FAILED(MFCreateSinkWriterFromURL(file.c_str(), nullptr, attributes.get(), writer.put()));
    com_ptr<IMFMediaType> output;
    THROW_IF_FAILED(MFCreateMediaType(output.put()));
    output->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    output->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
    output->SetUINT32(MF_MT_AVG_BITRATE, 8'000'000);
    output->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    output->SetUINT32(MF_MT_MPEG2_PROFILE, eAVEncH264VProfile_High);
    MFSetAttributeSize(output.get(), MF_MT_FRAME_SIZE, width, height);
    MFSetAttributeRatio(output.get(), MF_MT_FRAME_RATE, fps, 1);
    MFSetAttributeRatio(output.get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    DWORD stream = 0;
    THROW_IF_FAILED(writer->AddStream(output.get(), &stream));
    com_ptr<IMFMediaType> input;
    THROW_IF_FAILED(MFCreateMediaType(input.put()));
    input->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    input->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
    input->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    input->SetUINT32(MF_MT_DEFAULT_STRIDE, width * 4); // top-down
    MFSetAttributeSize(input.get(), MF_MT_FRAME_SIZE, width, height);
    MFSetAttributeRatio(input.get(), MF_MT_FRAME_RATE, fps, 1);
    MFSetAttributeRatio(input.get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    THROW_IF_FAILED(writer->SetInputMediaType(stream, input.get(), nullptr));
    THROW_IF_FAILED(writer->BeginWriting());

    const UINT frames = fps * seconds;
    const LONGLONG duration = 10'000'000LL / fps;
    const DWORD bytes = width * height * 4;
    for (UINT f = 0; f < frames; ++f)
    {
        draw(static_cast<float>(f) / static_cast<float>(frames));
        const auto pixels = surface.pixels();
        com_ptr<IMFMediaBuffer> buffer;
        THROW_IF_FAILED(MFCreateMemoryBuffer(bytes, buffer.put()));
        BYTE* data = nullptr;
        THROW_IF_FAILED(buffer->Lock(&data, nullptr, nullptr));
        std::copy(pixels.begin(), pixels.end(), data);
        THROW_IF_FAILED(buffer->Unlock());
        THROW_IF_FAILED(buffer->SetCurrentLength(bytes));
        com_ptr<IMFSample> sample;
        THROW_IF_FAILED(MFCreateSample(sample.put()));
        THROW_IF_FAILED(sample->AddBuffer(buffer.get()));
        THROW_IF_FAILED(sample->SetSampleTime(static_cast<LONGLONG>(f) * duration));
        THROW_IF_FAILED(sample->SetSampleDuration(duration));
        THROW_IF_FAILED(writer->WriteSample(stream, sample.get()));
    }
    THROW_IF_FAILED(writer->Finalize());
    std::wprintf(L"  %ls (%ux%u, %u s at %u fps)\n", file.filename().c_str(), width, height, seconds, fps);
}

// ---- Diva Stage: a concert stage, spotlights, an equalizer, a waveform ------------------------------

void drawDiva(Surface& s, float t)
{
    auto* dc = s.dc();
    const float w = s.width();
    const float h = s.height();
    const float u = h / 1080.0F;
    const Color base = hex(0x0B1026);
    const Color teal = hex(0x2CD5C4);
    const Color pink = hex(0xFF4FA3);
    const Color violet = hex(0x7B5CFF);
    const Color white = hex(0xE8FCFF);

    s.begin(base);
    dc->FillRectangle({0, 0, w, h},
                      linear(dc, {0, 0}, {0, h}, {{0, hex(0x070A1C)}, {0.55F, base}, {1, hex(0x1A0F3A)}}).get());
    blob(dc, {w * 0.5F, h * 1.05F}, w * 0.65F, h * 0.55F, withAlpha(violet, 0.45F));
    blob(dc, {w * 0.5F, h * 0.08F}, w * 0.35F, h * 0.25F, withAlpha(teal, 0.10F));

    // Spotlights from the rig, swaying.
    s.glow(40 * u, 0.8F, [&] {
        const std::array<Color, 4> colors = {teal, pink, violet, teal};
        for (int i = 0; i < 4; ++i)
        {
            const float x = w * (0.14F + (0.24F * static_cast<float>(i)));
            const float sway = 0.10F * std::sin(kTau * (t + (0.25F * static_cast<float>(i))));
            const D2D1_POINT_2F top{x, -h * 0.04F};
            const float floorX = x + (sway * h) + (w * (0.5F - (x / w)) * 0.25F);
            const std::array<D2D1_POINT_2F, 4> cone = {
                {{top.x - (6 * u), top.y}, {top.x + (6 * u), top.y}, {floorX + (w * 0.11F), h * 0.80F}, {floorX - (w * 0.11F), h * 0.80F}}};
            const Color c = colors[static_cast<std::size_t>(i)];
            dc->FillGeometry(polygon(dc, cone).get(),
                             linear(dc, top, {floorX, h * 0.8F}, {{0, withAlpha(c, 0.55F)}, {0.7F, withAlpha(c, 0.12F)}, {1, withAlpha(c, 0.0F)}})
                                 .get());
            blob(dc, {floorX, h * 0.80F}, w * 0.12F, h * 0.035F, withAlpha(c, 0.5F));
        }
    }, false);

    // The stage edge.
    dc->FillRectangle({0, h * 0.86F, w, h}, linear(dc, {0, h * 0.86F}, {0, h}, {{0, hex(0x0A0E22, 0.0F)}, {1, hex(0x05060F, 0.9F)}}).get());

    // A waveform across the hall.
    const auto wave = [&](float phase) {
        return path(dc, [&](ID2D1GeometrySink* sink) {
            constexpr int kSteps = 480;
            for (int i = 0; i <= kSteps; ++i)
            {
                const float x = w * static_cast<float>(i) / kSteps;
                const float k = x / w;
                const float envelope = std::sin(kPi * k);
                const float y = (h * 0.50F) + (envelope * h * 0.07F *
                                               ((0.6F * std::sin(kTau * ((3 * k) + t + phase))) +
                                                (0.3F * std::sin(kTau * ((7 * k) - (2 * t)))) +
                                                (0.1F * std::sin(kTau * ((17 * k) + (3 * t))))));
                if (i == 0)
                {
                    sink->BeginFigure({x, y}, D2D1_FIGURE_BEGIN_HOLLOW);
                }
                else
                {
                    sink->AddLine({x, y});
                }
            }
            sink->EndFigure(D2D1_FIGURE_END_OPEN);
        });
    };
    s.glow(14 * u, 1.6F, [&] {
        dc->DrawGeometry(wave(0).get(), solid(dc, withAlpha(teal, 0.9F)).get(), 3.0F * u);
        dc->DrawGeometry(wave(0.15F).get(), solid(dc, withAlpha(pink, 0.6F)).get(), 2.0F * u);
    });
    dc->DrawGeometry(wave(0).get(), solid(dc, white).get(), 1.2F * u);

    // The equalizer, and its reflection on the stage.
    constexpr int kBars = 56;
    const float left = w * 0.07F;
    const float span = w * 0.86F;
    const float slot = span / kBars;
    const float barWidth = slot * 0.62F;
    const float floor = h * 0.86F;
    Random random(7);
    std::array<float, kBars> levels{};
    for (int i = 0; i < kBars; ++i)
    {
        const float k = static_cast<float>(i) / kBars;
        const float a = random.range(0, 1);
        const float b = random.range(0, 1);
        const float shape = 0.35F + (0.65F * std::sin(kPi * (0.1F + (0.8F * k))));
        const float beat = (0.55F * (0.5F + (0.5F * std::sin(kTau * ((2 * t) + a))))) +
                           (0.45F * (0.5F + (0.5F * std::sin(kTau * ((3 * t) + b)))));
        levels[static_cast<std::size_t>(i)] = shape * (0.25F + (0.75F * beat));
    }
    const auto bars = [&](bool reflection) {
        for (int i = 0; i < kBars; ++i)
        {
            const float level = levels[static_cast<std::size_t>(i)];
            const float x = left + (slot * static_cast<float>(i)) + ((slot - barWidth) / 2);
            const float height = h * (0.03F + (0.25F * level));
            const D2D1_RECT_F rect = reflection ? D2D1_RECT_F{x, floor + (4 * u), x + barWidth, floor + (4 * u) + (height * 0.35F)}
                                                : D2D1_RECT_F{x, floor - height, x + barWidth, floor};
            const auto brush = reflection
                                   ? linear(dc, {0, rect.top}, {0, rect.bottom}, {{0, withAlpha(teal, 0.28F)}, {1, withAlpha(teal, 0)}})
                                   : linear(dc, {0, rect.bottom}, {0, rect.top}, {{0, teal}, {0.6F, mix(teal, pink, 0.6F)}, {1, pink}});
            dc->FillRoundedRectangle({rect, barWidth / 2, barWidth / 2}, brush.get());
        }
    };
    s.glow(10 * u, 1.3F, [&] { bars(false); });
    bars(true);

    // Sparkles in the air.
    Random sparkle(11);
    for (int i = 0; i < 140; ++i)
    {
        const float x = sparkle.range(0, w);
        const float y = sparkle.range(0, h * 0.62F);
        const float size = sparkle.range(1.0F, 3.2F) * u;
        const float phase = sparkle.next();
        const auto speed = static_cast<float>(1 + static_cast<int>(sparkle.range(0, 3)));
        const float alpha = 0.25F + (0.75F * std::abs(std::sin(kPi * ((speed * t) + phase))));
        const Color c = sparkle.next() < 0.5F ? white : (sparkle.next() < 0.5F ? teal : pink);
        const std::array<D2D1_POINT_2F, 8> star = {{{x, y - (size * 3)}, {x + (size * 0.6F), y - (size * 0.6F)}, {x + (size * 3), y},
                                                    {x + (size * 0.6F), y + (size * 0.6F)}, {x, y + (size * 3)},
                                                    {x - (size * 0.6F), y + (size * 0.6F)}, {x - (size * 3), y},
                                                    {x - (size * 0.6F), y - (size * 0.6F)}}};
        dc->FillGeometry(polygon(dc, star).get(), solid(dc, withAlpha(c, alpha)).get());
    }
    s.end();
}

// ---- Tokyo Midnight: a city at night in the rain, neon signs --------------------------------------

void drawTokyo(Surface& s, float t)
{
    auto* dc = s.dc();
    const float w = s.width();
    const float h = s.height();
    const float u = h / 1080.0F;
    const Color night = hex(0x1A1B26);
    const Color storm = hex(0x24283B);
    const Color fg = hex(0xC0CAF5);
    const Color blue = hex(0x7AA2F7);
    const Color purple = hex(0xBB9AF7);
    const Color cyan = hex(0x7DCFFF);
    const Color red = hex(0xF7768E);
    const Color amber = hex(0xE0AF68);
    const float horizon = h * 0.80F;

    s.begin(night);
    dc->FillRectangle({0, 0, w, horizon},
                      linear(dc, {0, 0}, {0, horizon}, {{0, hex(0x13141C)}, {0.55F, night}, {0.9F, storm}, {1, hex(0x2F3549)}}).get());
    blob(dc, {w * 0.30F, horizon}, w * 0.55F, h * 0.30F, withAlpha(purple, 0.16F));
    blob(dc, {w * 0.80F, horizon}, w * 0.40F, h * 0.22F, withAlpha(cyan, 0.10F));

    // The moon behind thin clouds.
    s.glow(30 * u, 0.9F, [&] { dc->FillEllipse({{w * 0.78F, h * 0.17F}, 46 * u, 46 * u}, solid(dc, withAlpha(fg, 0.92F)).get()); });
    blob(dc, {w * 0.70F, h * 0.20F}, w * 0.20F, h * 0.025F, withAlpha(storm, 0.8F));

    // Three rows of buildings, the nearest last; lit windows.
    struct Layer
    {
        Color body;
        float low;
        float high;
        float lit;
        std::uint64_t seed;
    };
    const std::array<Layer, 3> layers = {{{hex(0x1F2335), 0.12F, 0.30F, 0.08F, 21},
                                          {hex(0x181A26), 0.18F, 0.42F, 0.14F, 22},
                                          {hex(0x111219), 0.10F, 0.30F, 0.20F, 23}}};
    for (std::size_t li = 0; li < layers.size(); ++li)
    {
        const auto& layer = layers[li];
        Random random(layer.seed);
        float x = -random.range(0, 40 * u);
        while (x < w)
        {
            const float bw = random.range(70, 190) * u * (li == 2 ? 1.4F : 1.0F);
            const float bh = h * random.range(layer.low, layer.high);
            const D2D1_RECT_F body{x, horizon - bh, x + bw, horizon + (li == 2 ? h * 0.2F : 0)};
            dc->FillRectangle(body, solid(dc, layer.body).get());
            // Windows: a grid, some lit.
            const float cell = (li == 0 ? 10 : 14) * u;
            for (float wy = body.top + (cell * 1.2F); wy < horizon - cell; wy += cell * 1.6F)
            {
                for (float wx = body.left + (cell * 0.8F); wx < body.right - cell; wx += cell * 1.4F)
                {
                    if (random.next() < layer.lit)
                    {
                        const Color c = random.next() < 0.7F ? amber : (random.next() < 0.5F ? cyan : fg);
                        dc->FillRectangle({wx, wy, wx + (cell * 0.6F), wy + (cell * 0.75F)}, solid(dc, withAlpha(c, random.range(0.35F, 0.85F))).get());
                    }
                }
            }
            x += bw + random.range(0, 12) * u;
        }
    }

    // Two buildings in front carry the signs.
    const D2D1_RECT_F tower{w * 0.10F, horizon - (h * 0.52F), w * 0.19F, horizon};
    const D2D1_RECT_F shop{w * 0.57F, horizon - (h * 0.22F), w * 0.77F, horizon};
    Random lights(24);
    for (const auto& body : {tower, shop})
    {
        dc->FillRectangle(body, solid(dc, hex(0x0D0E15)).get());
        const float cell = 14 * u;
        for (float wy = body.top + (cell * 1.2F); wy < horizon - cell; wy += cell * 1.6F)
        {
            for (float wx = body.left + (cell * 0.8F); wx < body.right - cell; wx += cell * 1.4F)
            {
                if (lights.next() < 0.18F)
                {
                    dc->FillRectangle({wx, wy, wx + (cell * 0.6F), wy + (cell * 0.75F)}, solid(dc, withAlpha(amber, lights.range(0.3F, 0.8F))).get());
                }
            }
        }
    }

    // Neon: a vertical sign (東京) off the tower, a round one (夜), a shop's (ラーメン); they flicker
    // a little in the loop.
    const float flicker = 0.88F + (0.12F * std::sin(kTau * 5 * t)) * (0.5F + (0.5F * std::sin(kTau * 2 * t)));
    const float signX = tower.right + (58 * u);
    const float signTop = tower.top + (h * 0.06F);
    const D2D1_RECT_F sign{signX - (44 * u), signTop, signX + (44 * u), signTop + (240 * u)};
    const D2D1_RECT_F board{shop.left + (16 * u), shop.top - (70 * u), shop.right - (16 * u), shop.top - (8 * u)};
    const D2D1_POINT_2F round{w * 0.84F, horizon - (h * 0.34F)};
    const auto bracket = solid(dc, hex(0x2A2E42));
    dc->DrawLine({tower.right, signTop + (30 * u)}, {sign.left, signTop + (30 * u)}, bracket.get(), 5 * u);
    dc->DrawLine({tower.right, sign.bottom - (30 * u)}, {sign.left, sign.bottom - (30 * u)}, bracket.get(), 5 * u);
    dc->FillRoundedRectangle({sign, 14 * u, 14 * u}, solid(dc, hex(0x15121C, 0.9F)).get());
    dc->FillRoundedRectangle({board, 10 * u, 10 * u}, solid(dc, hex(0x15121C, 0.9F)).get());
    s.glow(16 * u, 1.8F * flicker, [&] {
        dc->DrawRoundedRectangle({sign, 14 * u, 14 * u}, solid(dc, withAlpha(red, flicker)).get(), 4 * u);
        text(s, L"東", L"Yu Gothic UI", 72 * u, {signX, signTop + (66 * u)}, withAlpha(red, flicker));
        text(s, L"京", L"Yu Gothic UI", 72 * u, {signX, signTop + (172 * u)}, withAlpha(red, flicker));
        dc->DrawEllipse({round, 62 * u, 62 * u}, solid(dc, cyan).get(), 4 * u);
        text(s, L"夜", L"Yu Gothic UI", 74 * u, round, cyan);
        dc->DrawRoundedRectangle({board, 10 * u, 10 * u}, solid(dc, purple).get(), 3.5F * u);
        text(s, L"ラーメン", L"Yu Gothic UI", 40 * u, {(board.left + board.right) / 2, (board.top + board.bottom) / 2}, purple);
    });

    // The wet street mirrors the lights.
    dc->FillRectangle({0, horizon, w, h}, linear(dc, {0, horizon}, {0, h}, {{0, hex(0x16161E)}, {1, hex(0x0D0E14)}}).get());
    s.glow(26 * u, 0.9F, [&] {
        dc->FillRectangle({sign.left + (20 * u), horizon + (8 * u), sign.right - (20 * u), h},
                          linear(dc, {0, horizon}, {0, h}, {{0, withAlpha(red, 0.55F * flicker)}, {1, withAlpha(red, 0)}}).get());
        dc->FillRectangle({round.x - (30 * u), horizon + (8 * u), round.x + (30 * u), h},
                          linear(dc, {0, horizon}, {0, h}, {{0, withAlpha(cyan, 0.45F)}, {1, withAlpha(cyan, 0)}}).get());
        dc->FillRectangle({board.left + (30 * u), horizon + (8 * u), board.right - (30 * u), horizon + (h * 0.12F)},
                          linear(dc, {0, horizon}, {0, horizon + (h * 0.12F)}, {{0, withAlpha(purple, 0.35F)}, {1, withAlpha(purple, 0)}}).get());
    }, false);

    // Rain: each drop crosses the picture a whole number of times per loop.
    Random rain(31);
    const auto brush = solid(dc, hex(0xA9B1D6));
    const float slant = 0.18F;
    for (int i = 0; i < 900; ++i)
    {
        const float length = rain.range(26, 64) * u;
        const auto laps = static_cast<float>(1 + static_cast<int>(rain.range(0, 3)));
        const float y = (fract(rain.next() + (laps * t)) * (h + length)) - length;
        const float x = rain.range(-0.1F * w, w) + (slant * y);
        brush->SetOpacity(rain.range(0.10F, 0.35F));
        dc->DrawLine({x, y}, {x + (slant * length), y + length}, brush.get(), rain.range(0.8F, 1.8F) * u);
    }
    // Mist over the street.
    dc->FillRectangle({0, horizon - (h * 0.08F), w, horizon + (h * 0.06F)},
                      linear(dc, {0, horizon - (h * 0.08F)}, {0, horizon + (h * 0.06F)},
                             {{0, withAlpha(blue, 0)}, {0.5F, withAlpha(blue, 0.10F)}, {1, withAlpha(blue, 0)}})
                          .get());
    s.end();
}

// ---- Sakura Dawn: a spring morning, a blossoming branch, falling petals ------------------------------

com_ptr<ID2D1PathGeometry> petal(ID2D1DeviceContext* dc)
{
    // Unit petal from (0,0) up to its notched tip at y = -1.
    return path(dc, [](ID2D1GeometrySink* sink) {
        sink->BeginFigure({0, 0}, D2D1_FIGURE_BEGIN_FILLED);
        sink->AddBezier({{-0.62F, -0.28F}, {-0.52F, -0.98F}, {-0.13F, -1.04F}});
        sink->AddLine({0, -0.90F});
        sink->AddLine({0.13F, -1.04F});
        sink->AddBezier({{0.52F, -0.98F}, {0.62F, -0.28F}, {0, 0}});
        sink->EndFigure(D2D1_FIGURE_END_CLOSED);
    });
}

void drawSakura(Surface& s, float /*t*/)
{
    auto* dc = s.dc();
    const float w = s.width();
    const float h = s.height();
    const float u = h / 1080.0F;
    const Color base = hex(0xFAF4ED);
    const Color surface = hex(0xFFFAF3);
    const Color overlay = hex(0xF2E9E1);
    const Color ink = hex(0x575279);
    const Color rose = hex(0xD7827E);
    const Color love = hex(0xB4637A);
    const Color blush = hex(0xEBBCBA);
    const Color gold = hex(0xEA9D34);
    const Color iris = hex(0x907AA9);

    s.begin(base);
    dc->FillRectangle({0, 0, w, h}, linear(dc, {0, 0}, {0, h}, {{0, surface}, {0.6F, base}, {1, overlay}}).get());
    blob(dc, {w * 0.70F, h * 0.30F}, h * 0.42F, h * 0.42F, withAlpha(gold, 0.20F));
    dc->FillEllipse({{w * 0.70F, h * 0.30F}, h * 0.09F, h * 0.09F}, solid(dc, withAlpha(hex(0xF6C177), 0.55F)).get());

    // Hills far away, softer the farther.
    const std::array<std::pair<Color, float>, 3> hills = {{{withAlpha(iris, 0.18F), 0.70F}, {withAlpha(rose, 0.16F), 0.78F}, {withAlpha(ink, 0.14F), 0.87F}}};
    Random random(41);
    for (const auto& [color, level] : hills)
    {
        const float a = random.range(0, kTau);
        dc->FillGeometry(path(dc, [&](ID2D1GeometrySink* sink) {
                             sink->BeginFigure({0, h}, D2D1_FIGURE_BEGIN_FILLED);
                             for (int i = 0; i <= 200; ++i)
                             {
                                 const float x = w * static_cast<float>(i) / 200;
                                 const float k = x / w;
                                 sink->AddLine({x, (h * level) - (h * 0.05F * (std::sin((kTau * 1.3F * k) + a) + (0.4F * std::sin((kTau * 3.1F * k) + (2 * a)))))});
                             }
                             sink->AddLine({w, h});
                             sink->EndFigure(D2D1_FIGURE_END_CLOSED);
                         }).get(),
                         solid(dc, color).get());
    }

    // A branch from the top-left corner, its twigs, and blossoms at their ends.
    const auto unitPetal = petal(dc);
    const auto flower = [&](D2D1_POINT_2F at, float size, float angle, Color c) {
        for (int p = 0; p < 5; ++p)
        {
            dc->SetTransform(D2D1::Matrix3x2F::Scale(size, size) * D2D1::Matrix3x2F::Rotation(angle + (72.0F * static_cast<float>(p))) *
                             D2D1::Matrix3x2F::Translation(at.x, at.y));
            dc->FillGeometry(unitPetal.get(), solid(dc, c).get());
            dc->DrawGeometry(unitPetal.get(), solid(dc, withAlpha(love, 0.35F)).get(), 0.6F / size * u);
        }
        dc->SetTransform(D2D1::Matrix3x2F::Identity());
        dc->FillEllipse({at, size * 0.18F, size * 0.18F}, solid(dc, love).get());
        Random stamen(static_cast<std::uint64_t>(at.x * 7 + at.y));
        for (int k = 0; k < 7; ++k)
        {
            const float a = stamen.range(0, kTau);
            const D2D1_POINT_2F tip{at.x + (std::cos(a) * size * 0.38F), at.y + (std::sin(a) * size * 0.38F)};
            dc->DrawLine(at, tip, solid(dc, withAlpha(love, 0.8F)).get(), 1.2F * u);
            dc->FillEllipse({tip, 2.2F * u, 2.2F * u}, solid(dc, gold).get());
        }
    };
    const auto bark = solid(dc, withAlpha(ink, 0.85F));
    std::vector<std::pair<D2D1_POINT_2F, float>> tips;
    const std::function<void(D2D1_POINT_2F, float, float, float, int)> grow = [&](D2D1_POINT_2F from, float angle, float length,
                                                                                    float thickness, int depth) {
        const float rad = angle * kPi / 180.0F;
        const D2D1_POINT_2F to{from.x + (std::cos(rad) * length), from.y + (std::sin(rad) * length)};
        const D2D1_POINT_2F bend{from.x + (std::cos(rad) * length * 0.5F) + (random.range(-0.12F, 0.12F) * length),
                                 from.y + (std::sin(rad) * length * 0.5F) + (random.range(-0.12F, 0.12F) * length)};
        com_ptr<ID2D1StrokeStyle> round;
        com_ptr<ID2D1Factory> factory;
        dc->GetFactory(factory.put());
        const D2D1_STROKE_STYLE_PROPERTIES props{D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND,
                                                 D2D1_LINE_JOIN_ROUND, 10, D2D1_DASH_STYLE_SOLID, 0};
        THROW_IF_FAILED(factory->CreateStrokeStyle(props, nullptr, 0, round.put()));
        dc->DrawGeometry(path(dc, [&](ID2D1GeometrySink* sink) {
                             sink->BeginFigure(from, D2D1_FIGURE_BEGIN_HOLLOW);
                             sink->AddQuadraticBezier({bend, to});
                             sink->EndFigure(D2D1_FIGURE_END_OPEN);
                         }).get(),
                         bark.get(), thickness, round.get());
        if (depth == 0)
        {
            tips.emplace_back(to, angle);
            return;
        }
        const int children = depth > 2 ? 2 : 3;
        for (int c = 0; c < children; ++c)
        {
            const float turn = random.range(-38, 38) + (c == 0 ? -12.0F : 12.0F);
            grow(to, angle + turn, length * random.range(0.55F, 0.75F), thickness * 0.62F, depth - 1);
        }
        if (depth <= 2)
        {
            tips.emplace_back(to, angle);
        }
    };
    grow({-20 * u, h * 0.08F}, 18, w * 0.16F, 26 * u, 4);
    grow({-20 * u, h * 0.30F}, -8, w * 0.10F, 14 * u, 3);
    const std::array<Color, 3> petals = {blush, withAlpha(rose, 0.9F), hex(0xF4D4CF)};
    for (const auto& [at, angle] : tips)
    {
        const int count = 1 + static_cast<int>(random.range(0, 3));
        for (int i = 0; i < count; ++i)
        {
            const D2D1_POINT_2F p{at.x + (random.range(-26, 26) * u), at.y + (random.range(-26, 26) * u)};
            flower(p, random.range(16, 30) * u, random.range(0, 72) + angle, petals[static_cast<std::size_t>(random.range(0, 2.99F))]);
        }
    }

    // Petals on the wind, more to the right and lower down.
    Random wind(43);
    for (int i = 0; i < 190; ++i)
    {
        const float x = w * std::pow(wind.next(), 0.7F);
        const float y = h * std::pow(wind.next(), 0.8F);
        const float size = wind.range(9, 24) * u;
        dc->SetTransform(D2D1::Matrix3x2F::Scale(size * 0.75F, size) * D2D1::Matrix3x2F::Rotation(wind.range(0, 360)) *
                         D2D1::Matrix3x2F::Translation(x, y));
        dc->FillGeometry(unitPetal.get(), solid(dc, withAlpha(petals[static_cast<std::size_t>(wind.range(0, 2.99F))], wind.range(0.65F, 0.95F))).get());
    }
    dc->SetTransform(D2D1::Matrix3x2F::Identity());
    s.end();
}

// ---- Mocha Kawaii: pastel lights, pixel stars, kaomoji ----------------------------------------------

void drawMocha(Surface& s, float /*t*/)
{
    auto* dc = s.dc();
    const float w = s.width();
    const float h = s.height();
    const float u = h / 1080.0F;
    const Color base = hex(0x1E1E2E);
    const Color pink = hex(0xF5C2E7);
    const Color mauve = hex(0xCBA6F7);
    const Color teal = hex(0x94E2D5);
    const Color peach = hex(0xFAB387);
    const Color yellow = hex(0xF9E2AF);
    const Color sky = hex(0x89DCEB);
    const Color ink = hex(0xCDD6F4);
    const Color red = hex(0xF38BA8);

    s.begin(base);
    dc->FillRectangle({0, 0, w, h}, linear(dc, {0, 0}, {w, h}, {{0, hex(0x181825)}, {0.5F, base}, {1, hex(0x232336)}}).get());
    blob(dc, {w * 0.18F, h * 0.22F}, w * 0.38F, h * 0.50F, withAlpha(pink, 0.30F));
    blob(dc, {w * 0.82F, h * 0.28F}, w * 0.34F, h * 0.48F, withAlpha(mauve, 0.30F));
    blob(dc, {w * 0.62F, h * 0.92F}, w * 0.40F, h * 0.40F, withAlpha(teal, 0.22F));
    blob(dc, {w * 0.12F, h * 0.90F}, w * 0.30F, h * 0.35F, withAlpha(peach, 0.20F));

    // A big soft moon with a face.
    const D2D1_POINT_2F moon{w * 0.5F, h * 0.46F};
    const float r = h * 0.17F;
    s.glow(40 * u, 0.7F, [&] { dc->FillEllipse({moon, r, r}, solid(dc, hex(0xF2DCEC)).get()); });
    dc->FillEllipse({{moon.x - (r * 0.38F), moon.y + (r * 0.22F)}, r * 0.13F, r * 0.08F}, solid(dc, withAlpha(red, 0.45F)).get());
    dc->FillEllipse({{moon.x + (r * 0.38F), moon.y + (r * 0.22F)}, r * 0.13F, r * 0.08F}, solid(dc, withAlpha(red, 0.45F)).get());
    const auto eye = solid(dc, hex(0x45475A));
    dc->FillEllipse({{moon.x - (r * 0.30F), moon.y - (r * 0.02F)}, r * 0.07F, r * 0.10F}, eye.get());
    dc->FillEllipse({{moon.x + (r * 0.30F), moon.y - (r * 0.02F)}, r * 0.07F, r * 0.10F}, eye.get());
    dc->FillEllipse({{moon.x - (r * 0.28F), moon.y - (r * 0.06F)}, r * 0.025F, r * 0.03F}, solid(dc, hex(0xFFFFFF)).get());
    dc->FillEllipse({{moon.x + (r * 0.32F), moon.y - (r * 0.06F)}, r * 0.025F, r * 0.03F}, solid(dc, hex(0xFFFFFF)).get());
    dc->DrawGeometry(path(dc, [&](ID2D1GeometrySink* sink) {
                         sink->BeginFigure({moon.x - (r * 0.12F), moon.y + (r * 0.16F)}, D2D1_FIGURE_BEGIN_HOLLOW);
                         sink->AddBezier({{moon.x - (r * 0.08F), moon.y + (r * 0.26F)}, {moon.x, moon.y + (r * 0.24F)}, {moon.x, moon.y + (r * 0.17F)}});
                         sink->AddBezier({{moon.x, moon.y + (r * 0.24F)}, {moon.x + (r * 0.08F), moon.y + (r * 0.26F)}, {moon.x + (r * 0.12F), moon.y + (r * 0.16F)}});
                         sink->EndFigure(D2D1_FIGURE_END_OPEN);
                     }).get(),
                     eye.get(), r * 0.03F);

    // Pixel art: stars (a plus with a dot in each corner) and hearts.
    Random random(51);
    const std::array<Color, 6> colors = {yellow, pink, sky, ink, mauve, teal};
    const auto pixel = [&](float x, float y, float p, Color c) { dc->FillRectangle({x, y, x + p, y + p}, solid(dc, c).get()); };
    for (int i = 0; i < 90; ++i)
    {
        const float x = random.range(0, w);
        const float y = random.range(0, h);
        if (std::hypot(x - moon.x, y - moon.y) < r * 1.5F)
        {
            continue;
        }
        const float p = std::round(random.range(3, 7)) * u;
        const Color c = withAlpha(random.pick<Color>(colors), random.range(0.6F, 1.0F));
        if (random.next() < 0.75F)
        {
            pixel(x, y, p, c);
            pixel(x - p, y, p, c);
            pixel(x + p, y, p, c);
            pixel(x, y - p, p, c);
            pixel(x, y + p, p, c);
            if (p > 4.5F * u)
            {
                pixel(x, y - (2 * p), p, withAlpha(c, c.a * 0.6F));
                pixel(x, y + (2 * p), p, withAlpha(c, c.a * 0.6F));
                pixel(x - (2 * p), y, p, withAlpha(c, c.a * 0.6F));
                pixel(x + (2 * p), y, p, withAlpha(c, c.a * 0.6F));
            }
        }
        else
        {
            // ♡ in 7x6 pixels.
            constexpr std::array<std::string_view, 6> kHeart = {".XX.XX.", "XXXXXXX", "XXXXXXX", ".XXXXX.", "..XXX..", "...X..."};
            const Color heart = withAlpha(random.next() < 0.6F ? pink : red, 0.9F);
            for (std::size_t row = 0; row < kHeart.size(); ++row)
            {
                for (std::size_t col = 0; col < kHeart[row].size(); ++col)
                {
                    if (kHeart[row][col] == 'X')
                    {
                        pixel(x + (static_cast<float>(col) * p), y + (static_cast<float>(row) * p), p, heart);
                    }
                }
            }
        }
    }

    // Kaomoji around the moon.
    const std::array<std::wstring_view, 10> faces = {L"(｡•ᴗ•｡)",  L"ʕ•ᴥ•ʔ",     L"(◕‿◕)",   L"(=^･ω･^=)", L"☆彡",
                                                     L"( ˘ω˘ )", L"(っ◔◡◔)っ ♡", L"(✿◠‿◠)", L"ฅ^•ﻌ•^ฅ",  L"(ﾉ◕ヮ◕)ﾉ*:･ﾟ✧"};
    const std::array<D2D1_POINT_2F, 10> places = {{{0.16F, 0.18F}, {0.84F, 0.16F}, {0.30F, 0.70F}, {0.72F, 0.74F}, {0.50F, 0.12F},
                                                   {0.10F, 0.52F}, {0.88F, 0.52F}, {0.20F, 0.86F}, {0.82F, 0.90F}, {0.52F, 0.88F}}};
    for (std::size_t i = 0; i < faces.size(); ++i)
    {
        const Color c = colors[i % colors.size()];
        text(s, faces[i], L"Segoe UI", random.range(34, 52) * u, {w * places[i].x, h * places[i].y}, withAlpha(c, 0.9F),
             DWRITE_FONT_WEIGHT_SEMI_BOLD, random.range(-12, 12));
    }
    s.end();
}

// ---- Your Rice: the kit's preview (the rice itself shows your own picture) ------------------------------

void drawKit(Surface& s, float t)
{
    drawDiva(s, t);
}

// ---- cursors ----------------------------------------------------------------------------------------------

struct CursorStyle
{
    Color fill;
    Color outline;
    Color accent;
};

enum class Shape : std::uint8_t
{
    Arrow,
    Help,
    Working,
    Busy,
    Cross,
    Beam,
    Pen,
    No,
    SizeNS,
    SizeWE,
    SizeNWSE,
    SizeNESW,
    Move,
    Up,
    Hand,
    Pin,
    Person,
};

struct CursorSpec
{
    std::string_view role; // rice.toml [cursors.map] name
    std::string_view file;
    Shape shape;
    D2D1_POINT_2F hotspot; // in the 32-unit design grid
    bool animated;
};

constexpr std::array<CursorSpec, 17> kCursors = {{
    {"normal", "normal.cur", Shape::Arrow, {3, 2}, false},
    {"help", "help.cur", Shape::Help, {3, 2}, false},
    {"working", "working.ani", Shape::Working, {3, 2}, true},
    {"busy", "busy.ani", Shape::Busy, {16, 16}, true},
    {"precision", "precision.cur", Shape::Cross, {16, 16}, false},
    {"text", "text.cur", Shape::Beam, {16, 16}, false},
    {"handwriting", "handwriting.cur", Shape::Pen, {4, 28}, false},
    {"unavailable", "unavailable.cur", Shape::No, {16, 16}, false},
    {"vertical", "vertical.cur", Shape::SizeNS, {16, 16}, false},
    {"horizontal", "horizontal.cur", Shape::SizeWE, {16, 16}, false},
    {"diagonal1", "diagonal1.cur", Shape::SizeNWSE, {16, 16}, false},
    {"diagonal2", "diagonal2.cur", Shape::SizeNESW, {16, 16}, false},
    {"move", "move.cur", Shape::Move, {16, 16}, false},
    {"alternate", "alternate.cur", Shape::Up, {16, 3}, false},
    {"link", "link.cur", Shape::Hand, {12, 3}, false},
    {"location", "location.cur", Shape::Pin, {3, 2}, false},
    {"person", "person.cur", Shape::Person, {3, 2}, false},
}};

constexpr std::array<UINT, 4> kCursorSizes = {32, 48, 64, 96};
constexpr std::array<UINT, 3> kAniSizes = {32, 48, 64};
constexpr int kAniFrames = 12;

// One cursor picture in the 32-unit grid (the caller scales it); `t` turns the spinners.
void drawShape(Surface& s, Shape shape, const CursorStyle& style, float t)
{
    auto* dc = s.dc();
    const auto fill = solid(dc, style.fill);
    const auto line = solid(dc, style.outline);
    const auto accent = solid(dc, style.accent);
    com_ptr<ID2D1Factory> factory;
    dc->GetFactory(factory.put());
    com_ptr<ID2D1StrokeStyle> round;
    const D2D1_STROKE_STYLE_PROPERTIES props{D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND,
                                             D2D1_LINE_JOIN_ROUND, 10, D2D1_DASH_STYLE_SOLID, 0};
    THROW_IF_FAILED(factory->CreateStrokeStyle(props, nullptr, 0, round.put()));
    const float stroke = 1.6F;
    const auto outlined = [&](ID2D1Geometry* g, ID2D1Brush* inside = nullptr) {
        dc->FillGeometry(g, inside != nullptr ? inside : fill.get());
        dc->DrawGeometry(g, line.get(), stroke, round.get());
    };
    const auto arrow = [&](float scale = 1.0F) {
        const std::array<D2D1_POINT_2F, 7> points = {{{3, 2}, {3, 2 + (21 * scale)}, {3 + (5.2F * scale), 2 + (16.5F * scale)},
                                                      {3 + (9 * scale), 2 + (24.5F * scale)}, {3 + (12.5F * scale), 2 + (23 * scale)},
                                                      {3 + (8.8F * scale), 2 + (15 * scale)}, {3 + (15.5F * scale), 2 + (15 * scale)}}};
        outlined(polygon(dc, points).get());
    };
    // A double-headed arrow along +x, centered at (16,16), turned by `angle` degrees.
    const auto twoWay = [&](float angle) {
        const std::array<D2D1_POINT_2F, 10> points = {{{4, 16}, {10, 10}, {10, 14}, {22, 14}, {22, 10}, {28, 16}, {22, 22}, {22, 18}, {10, 18}, {10, 22}}};
        D2D1_MATRIX_3X2_F before;
        dc->GetTransform(&before);
        dc->SetTransform(D2D1::Matrix3x2F::Rotation(angle, {16, 16}) * before);
        outlined(polygon(dc, points).get());
        dc->SetTransform(before);
    };
    const auto spinner = [&](D2D1_POINT_2F center, float radius, float width) {
        dc->DrawEllipse({center, radius, radius}, line.get(), width + (2 * stroke));
        dc->DrawEllipse({center, radius, radius}, fill.get(), width);
        const float start = kTau * t;
        const float sweep = kPi * 0.65F;
        dc->DrawGeometry(path(dc, [&](ID2D1GeometrySink* sink) {
                             sink->BeginFigure({center.x + (radius * std::cos(start)), center.y + (radius * std::sin(start))}, D2D1_FIGURE_BEGIN_HOLLOW);
                             sink->AddArc({{center.x + (radius * std::cos(start + sweep)), center.y + (radius * std::sin(start + sweep))},
                                           {radius, radius}, 0, D2D1_SWEEP_DIRECTION_CLOCKWISE, D2D1_ARC_SIZE_SMALL});
                             sink->EndFigure(D2D1_FIGURE_END_OPEN);
                         }).get(),
                         accent.get(), width, round.get());
    };
    switch (shape)
    {
    case Shape::Arrow:
        arrow();
        break;
    case Shape::Help:
        arrow(0.9F);
        dc->FillEllipse({{24, 22}, 7, 7}, accent.get());
        dc->DrawEllipse({{24, 22}, 7, 7}, line.get(), stroke);
        text(s, L"?", L"Segoe UI", 11, {24, 22}, style.outline);
        break;
    case Shape::Working:
        arrow(0.85F);
        spinner({23, 23}, 5.5F, 2.6F);
        break;
    case Shape::Busy:
        spinner({16, 16}, 10, 4.2F);
        break;
    case Shape::Cross:
        for (const auto& [a, b] : {std::pair<D2D1_POINT_2F, D2D1_POINT_2F>{{16, 3}, {16, 12}}, {{16, 20}, {16, 29}}, {{3, 16}, {12, 16}}, {{20, 16}, {29, 16}}})
        {
            dc->DrawLine(a, b, line.get(), 2.0F + (2 * stroke), round.get());
            dc->DrawLine(a, b, fill.get(), 2.0F, round.get());
        }
        dc->FillEllipse({{16, 16}, 1.6F, 1.6F}, accent.get());
        break;
    case Shape::Beam:
        for (ID2D1Brush* brush : {static_cast<ID2D1Brush*>(line.get()), static_cast<ID2D1Brush*>(fill.get())})
        {
            const float wide = brush == line.get() ? 2.0F + (2 * stroke) : 2.0F;
            dc->DrawLine({16, 6}, {16, 26}, brush, wide, round.get());
            dc->DrawLine({12, 5}, {20, 5}, brush, wide, round.get());
            dc->DrawLine({12, 27}, {20, 27}, brush, wide, round.get());
        }
        break;
    case Shape::Pen:
    {
        const std::array<D2D1_POINT_2F, 5> pen = {{{4, 28}, {6, 21}, {22, 5}, {27, 10}, {11, 26}}};
        outlined(polygon(dc, pen).get());
        const std::array<D2D1_POINT_2F, 3> nib = {{{4, 28}, {6, 21}, {11, 26}}};
        outlined(polygon(dc, nib).get(), accent.get());
        break;
    }
    case Shape::No:
        dc->DrawEllipse({{16, 16}, 11, 11}, line.get(), 3.0F + (2 * stroke));
        dc->DrawLine({8.5F, 23.5F}, {23.5F, 8.5F}, line.get(), 3.0F + (2 * stroke));
        dc->DrawEllipse({{16, 16}, 11, 11}, accent.get(), 3.0F);
        dc->DrawLine({8.5F, 23.5F}, {23.5F, 8.5F}, accent.get(), 3.0F);
        break;
    case Shape::SizeNS:
        twoWay(90);
        break;
    case Shape::SizeWE:
        twoWay(0);
        break;
    case Shape::SizeNWSE:
        twoWay(45);
        break;
    case Shape::SizeNESW:
        twoWay(-45);
        break;
    case Shape::Move:
    {
        const std::array<D2D1_POINT_2F, 20> points = {{{16, 2}, {21, 7}, {18, 7}, {18, 14}, {25, 14}, {25, 11}, {30, 16}, {25, 21}, {25, 18}, {18, 18},
                                                       {18, 25}, {21, 25}, {16, 30}, {11, 25}, {14, 25}, {14, 18}, {7, 18}, {7, 21}, {2, 16}, {7, 11}}};
        auto g = path(dc, [&](ID2D1GeometrySink* sink) {
            sink->BeginFigure(points[0], D2D1_FIGURE_BEGIN_FILLED);
            for (std::size_t i = 1; i < points.size(); ++i)
            {
                sink->AddLine(points[i]);
            }
            sink->AddLine({7, 14});
            sink->AddLine({14, 14});
            sink->AddLine({14, 7});
            sink->AddLine({11, 7});
            sink->EndFigure(D2D1_FIGURE_END_CLOSED);
        });
        outlined(g.get());
        break;
    }
    case Shape::Up:
    {
        const std::array<D2D1_POINT_2F, 7> points = {{{16, 3}, {25, 13}, {19, 13}, {19, 28}, {13, 28}, {13, 13}, {7, 13}}};
        outlined(polygon(dc, points).get());
        break;
    }
    case Shape::Hand:
    {
        // A pointing hand: the index finger up, three folded fingers, the thumb.
        auto g = path(dc, [](ID2D1GeometrySink* sink) {
            sink->BeginFigure({10, 4.5F}, D2D1_FIGURE_BEGIN_FILLED);
            sink->AddArc({{14, 4.5F}, {2, 2}, 0, D2D1_SWEEP_DIRECTION_CLOCKWISE, D2D1_ARC_SIZE_SMALL});
            sink->AddLine({14, 13});
            sink->AddArc({{18, 13.5F}, {2, 2}, 0, D2D1_SWEEP_DIRECTION_CLOCKWISE, D2D1_ARC_SIZE_SMALL});
            sink->AddArc({{22, 14.5F}, {2, 2}, 0, D2D1_SWEEP_DIRECTION_CLOCKWISE, D2D1_ARC_SIZE_SMALL});
            sink->AddArc({{26, 16}, {2, 2}, 0, D2D1_SWEEP_DIRECTION_CLOCKWISE, D2D1_ARC_SIZE_SMALL});
            sink->AddLine({26, 22});
            sink->AddBezier({{26, 27}, {23, 30}, {18, 30}});
            sink->AddLine({15, 30});
            sink->AddBezier({{11, 30}, {9, 28}, {7, 24}});
            sink->AddLine({4, 18.5F});
            sink->AddArc({{7.5F, 16.5F}, {2, 2}, 0, D2D1_SWEEP_DIRECTION_CLOCKWISE, D2D1_ARC_SIZE_SMALL});
            sink->AddLine({10, 19});
            sink->EndFigure(D2D1_FIGURE_END_CLOSED);
        });
        outlined(g.get());
        dc->DrawLine({18, 15.5F}, {18, 19}, line.get(), 1.0F, round.get());
        dc->DrawLine({22, 16.5F}, {22, 19}, line.get(), 1.0F, round.get());
        break;
    }
    case Shape::Pin:
    {
        arrow(0.85F);
        auto g = path(dc, [](ID2D1GeometrySink* sink) {
            sink->BeginFigure({24, 30}, D2D1_FIGURE_BEGIN_FILLED);
            sink->AddBezier({{20, 25}, {18, 22}, {18, 19.5F}});
            sink->AddArc({{30, 19.5F}, {6, 6}, 0, D2D1_SWEEP_DIRECTION_CLOCKWISE, D2D1_ARC_SIZE_SMALL});
            sink->AddBezier({{30, 22}, {28, 25}, {24, 30}});
            sink->EndFigure(D2D1_FIGURE_END_CLOSED);
        });
        outlined(g.get(), accent.get());
        dc->FillEllipse({{24, 19.5F}, 2.2F, 2.2F}, fill.get());
        break;
    }
    case Shape::Person:
        arrow(0.85F);
        dc->FillEllipse({{24, 17.5F}, 3.6F, 3.6F}, accent.get());
        dc->DrawEllipse({{24, 17.5F}, 3.6F, 3.6F}, line.get(), stroke);
        {
            auto g = path(dc, [](ID2D1GeometrySink* sink) {
                sink->BeginFigure({17.5F, 30}, D2D1_FIGURE_BEGIN_FILLED);
                sink->AddBezier({{17.5F, 25}, {20, 22.5F}, {24, 22.5F}});
                sink->AddBezier({{28, 22.5F}, {30.5F, 25}, {30.5F, 30}});
                sink->EndFigure(D2D1_FIGURE_END_CLOSED);
            });
            outlined(g.get(), accent.get());
        }
        break;
    }
}

// The picture of one cursor at `size` pixels, straight (not premultiplied) BGRA, top-down.
std::vector<std::uint8_t> cursorPixels(Gpu& gpu, const CursorSpec& spec, const CursorStyle& style, UINT size, float t)
{
    Surface s(gpu, size, size);
    s.begin({0, 0, 0, 0});
    const float scale = static_cast<float>(size) / 32.0F;
    s.dc()->SetTransform(D2D1::Matrix3x2F::Scale(scale, scale));
    drawShape(s, spec.shape, style, t);
    s.end();
    auto pixels = s.pixels();
    for (std::size_t i = 0; i < pixels.size(); i += 4)
    {
        const std::uint8_t a = pixels[i + 3];
        for (std::size_t c = 0; c < 3 && a != 0 && a != 255; ++c)
        {
            pixels[i + c] = static_cast<std::uint8_t>(std::min(255, (pixels[i + c] * 255 + (a / 2)) / a));
        }
    }
    return pixels;
}

void put16(std::vector<std::uint8_t>& out, std::uint32_t v)
{
    out.push_back(static_cast<std::uint8_t>(v & 0xFFU));
    out.push_back(static_cast<std::uint8_t>((v >> 8U) & 0xFFU));
}

void put32(std::vector<std::uint8_t>& out, std::uint32_t v)
{
    put16(out, v & 0xFFFFU);
    put16(out, v >> 16U);
}

void putTag(std::vector<std::uint8_t>& out, std::string_view tag)
{
    out.insert(out.end(), tag.begin(), tag.end());
}

// A .cur: one 32-bit DIB per size (with its AND mask: set where the pixel is transparent).
std::vector<std::uint8_t> curFile(Gpu& gpu, const CursorSpec& spec, const CursorStyle& style, std::span<const UINT> sizes, float t)
{
    std::vector<std::vector<std::uint8_t>> images;
    for (const UINT size : sizes)
    {
        const auto pixels = cursorPixels(gpu, spec, style, size, t);
        std::vector<std::uint8_t> dib;
        const UINT maskStride = ((size + 31) / 32) * 4;
        put32(dib, 40);
        put32(dib, size);
        put32(dib, size * 2);
        put16(dib, 1);
        put16(dib, 32);
        put32(dib, 0);
        put32(dib, (size * size * 4) + (maskStride * size));
        put32(dib, 0);
        put32(dib, 0);
        put32(dib, 0);
        put32(dib, 0);
        for (UINT y = size; y-- > 0;) // bottom-up
        {
            const auto* row = pixels.data() + (static_cast<std::size_t>(y) * size * 4);
            dib.insert(dib.end(), row, row + (static_cast<std::size_t>(size) * 4));
        }
        for (UINT y = size; y-- > 0;)
        {
            std::vector<std::uint8_t> mask(maskStride, 0);
            for (UINT x = 0; x < size; ++x)
            {
                if (pixels[((static_cast<std::size_t>(y) * size) + x) * 4 + 3] == 0)
                {
                    mask[x / 8] |= static_cast<std::uint8_t>(0x80U >> (x % 8));
                }
            }
            dib.insert(dib.end(), mask.begin(), mask.end());
        }
        images.push_back(std::move(dib));
    }
    std::vector<std::uint8_t> out;
    put16(out, 0);
    put16(out, 2); // cursor
    put16(out, static_cast<std::uint32_t>(images.size()));
    std::uint32_t offset = 6 + (16 * static_cast<std::uint32_t>(images.size()));
    for (std::size_t i = 0; i < images.size(); ++i)
    {
        const UINT size = sizes[i];
        const float scale = static_cast<float>(size) / 32.0F;
        out.push_back(static_cast<std::uint8_t>(size >= 256 ? 0 : size));
        out.push_back(static_cast<std::uint8_t>(size >= 256 ? 0 : size));
        out.push_back(0);
        out.push_back(0);
        put16(out, static_cast<std::uint32_t>(std::lround(spec.hotspot.x * scale)));
        put16(out, static_cast<std::uint32_t>(std::lround(spec.hotspot.y * scale)));
        put32(out, static_cast<std::uint32_t>(images[i].size()));
        put32(out, offset);
        offset += static_cast<std::uint32_t>(images[i].size());
    }
    for (const auto& image : images)
    {
        out.insert(out.end(), image.begin(), image.end());
    }
    return out;
}

// A .ani: RIFF "ACON" with its header and one .cur per frame.
std::vector<std::uint8_t> aniFile(Gpu& gpu, const CursorSpec& spec, const CursorStyle& style)
{
    std::vector<std::uint8_t> frames;
    putTag(frames, "fram");
    for (int f = 0; f < kAniFrames; ++f)
    {
        const auto cur = curFile(gpu, spec, style, kAniSizes, static_cast<float>(f) / kAniFrames);
        putTag(frames, "icon");
        put32(frames, static_cast<std::uint32_t>(cur.size()));
        frames.insert(frames.end(), cur.begin(), cur.end());
        if ((cur.size() % 2) != 0)
        {
            frames.push_back(0);
        }
    }
    std::vector<std::uint8_t> body;
    putTag(body, "ACON");
    putTag(body, "anih");
    put32(body, 36);
    put32(body, 36);
    put32(body, kAniFrames); // frames
    put32(body, kAniFrames); // steps
    put32(body, 0);
    put32(body, 0);
    put32(body, 0);
    put32(body, 0);
    put32(body, 4); // jiffies (1/60 s) a step: 15 fps
    put32(body, 1); // AF_ICON: the frames are icons
    putTag(body, "LIST");
    put32(body, static_cast<std::uint32_t>(frames.size()));
    body.insert(body.end(), frames.begin(), frames.end());
    std::vector<std::uint8_t> out;
    putTag(out, "RIFF");
    put32(out, static_cast<std::uint32_t>(body.size()));
    out.insert(out.end(), body.begin(), body.end());
    return out;
}

void writeBytes(const fs::path& file, std::span<const std::uint8_t> bytes)
{
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!out)
    {
        THROW_HR(E_FAIL);
    }
}

void saveCursors(Gpu& gpu, const fs::path& folder, const CursorStyle& style)
{
    fs::create_directories(folder);
    for (const auto& spec : kCursors)
    {
        writeBytes(folder / fs::path(spec.file), spec.animated ? aniFile(gpu, spec, style) : curFile(gpu, spec, style, kCursorSizes, 0));
    }
    std::wprintf(L"  cursors (%zu)\n", kCursors.size());
}

// ---- the app's icon ---------------------------------------------------------------------------------------

// A squircle in the Diva Stage colors, a sparkle, and a little bar of your own along the bottom.
void drawIcon(Surface& s)
{
    auto* dc = s.dc();
    const float n = s.width();
    s.begin({0, 0, 0, 0});
    const float inset = n * 0.04F;
    const D2D1_ROUNDED_RECT tile{{inset, inset, n - inset, n - inset}, n * 0.24F, n * 0.24F};
    dc->FillRoundedRectangle(tile, linear(dc, {0, 0}, {n, n}, {{0, hex(0x2CD5C4)}, {0.55F, hex(0x7B5CFF)}, {1, hex(0xFF4FA3)}}).get());
    dc->FillRoundedRectangle(tile, radial(dc, {n * 0.3F, n * 0.25F}, n * 0.6F, n * 0.6F, {{0, hex(0xFFFFFF, 0.28F)}, {1, hex(0xFFFFFF, 0)}}).get());
    const auto white = solid(dc, hex(0xFFFFFF));
    // The sparkle.
    const D2D1_POINT_2F c{n * 0.5F, n * 0.42F};
    const float r = n * 0.25F;
    const float k = n * 0.045F;
    dc->FillGeometry(path(dc, [&](ID2D1GeometrySink* sink) {
                         sink->BeginFigure({c.x, c.y - r}, D2D1_FIGURE_BEGIN_FILLED);
                         sink->AddQuadraticBezier({{c.x + k, c.y - k}, {c.x + r, c.y}});
                         sink->AddQuadraticBezier({{c.x + k, c.y + k}, {c.x, c.y + r}});
                         sink->AddQuadraticBezier({{c.x - k, c.y + k}, {c.x - r, c.y}});
                         sink->AddQuadraticBezier({{c.x - k, c.y - k}, {c.x, c.y - r}});
                         sink->EndFigure(D2D1_FIGURE_END_CLOSED);
                     }).get(),
                     white.get());
    dc->FillEllipse({{n * 0.74F, n * 0.24F}, n * 0.045F, n * 0.045F}, white.get());
    // The bar, with three icons on it.
    const D2D1_RECT_F bar{n * 0.18F, n * 0.72F, n * 0.82F, n * 0.84F};
    dc->FillRoundedRectangle({bar, n * 0.06F, n * 0.06F}, white.get());
    for (int i = 0; i < 3; ++i)
    {
        const float x = n * (0.30F + (0.20F * static_cast<float>(i)));
        dc->FillRoundedRectangle({{x - (n * 0.04F), n * 0.74F, x + (n * 0.04F), n * 0.82F}, n * 0.02F, n * 0.02F},
                                 solid(dc, i == 1 ? hex(0xFF4FA3) : hex(0x7B5CFF)).get());
    }
    s.end();
}

// A PNG with its alpha (straight), in memory.
std::vector<std::uint8_t> pngWithAlpha(Surface& surface)
{
    auto pixels = surface.pixels();
    for (std::size_t i = 0; i < pixels.size(); i += 4)
    {
        const std::uint8_t a = pixels[i + 3];
        for (std::size_t c = 0; c < 3 && a != 0 && a != 255; ++c)
        {
            pixels[i + c] = static_cast<std::uint8_t>(std::min(255, (pixels[i + c] * 255 + (a / 2)) / a));
        }
    }
    const auto width = static_cast<UINT>(surface.width());
    const auto height = static_cast<UINT>(surface.height());
    auto& wic = surface.gpu().wic;
    com_ptr<IStream> stream;
    THROW_IF_FAILED(CreateStreamOnHGlobal(nullptr, TRUE, stream.put()));
    com_ptr<IWICBitmapEncoder> encoder;
    THROW_IF_FAILED(wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, encoder.put()));
    THROW_IF_FAILED(encoder->Initialize(stream.get(), WICBitmapEncoderNoCache));
    com_ptr<IWICBitmapFrameEncode> frame;
    THROW_IF_FAILED(encoder->CreateNewFrame(frame.put(), nullptr));
    THROW_IF_FAILED(frame->Initialize(nullptr));
    THROW_IF_FAILED(frame->SetSize(width, height));
    WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
    THROW_IF_FAILED(frame->SetPixelFormat(&format));
    THROW_IF_FAILED(frame->WritePixels(height, width * 4, static_cast<UINT>(pixels.size()), pixels.data()));
    THROW_IF_FAILED(frame->Commit());
    THROW_IF_FAILED(encoder->Commit());
    STATSTG stat{};
    THROW_IF_FAILED(stream->Stat(&stat, STATFLAG_NONAME));
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(stat.cbSize.QuadPart));
    const LARGE_INTEGER start{};
    THROW_IF_FAILED(stream->Seek(start, STREAM_SEEK_SET, nullptr));
    ULONG read = 0;
    THROW_IF_FAILED(stream->Read(bytes.data(), static_cast<ULONG>(bytes.size()), &read));
    return bytes;
}

void put16(std::vector<std::uint8_t>& out, std::uint32_t v);
void put32(std::vector<std::uint8_t>& out, std::uint32_t v);
void writeBytes(const fs::path& file, std::span<const std::uint8_t> bytes);

// rmw.ico (PNG images, 16..256 px) and icon.png (256 px, for the window).
void saveIcon(Gpu& gpu, const fs::path& folder)
{
    constexpr std::array<UINT, 10> kSizes = {16, 20, 24, 32, 40, 48, 64, 96, 128, 256};
    std::vector<std::vector<std::uint8_t>> images;
    for (const UINT size : kSizes)
    {
        Surface s(gpu, size, size);
        drawIcon(s);
        images.push_back(pngWithAlpha(s));
    }
    std::vector<std::uint8_t> ico;
    put16(ico, 0);
    put16(ico, 1); // icon
    put16(ico, static_cast<std::uint32_t>(images.size()));
    std::uint32_t offset = 6 + (16 * static_cast<std::uint32_t>(images.size()));
    for (std::size_t i = 0; i < images.size(); ++i)
    {
        const UINT size = kSizes[i];
        ico.push_back(static_cast<std::uint8_t>(size >= 256 ? 0 : size));
        ico.push_back(static_cast<std::uint8_t>(size >= 256 ? 0 : size));
        ico.push_back(0);
        ico.push_back(0);
        put16(ico, 1);  // planes
        put16(ico, 32); // bits
        put32(ico, static_cast<std::uint32_t>(images[i].size()));
        put32(ico, offset);
        offset += static_cast<std::uint32_t>(images[i].size());
    }
    for (const auto& image : images)
    {
        ico.insert(ico.end(), image.begin(), image.end());
    }
    fs::create_directories(folder);
    writeBytes(folder / L"rmw.ico", ico);
    writeBytes(folder / L"icon.png", images.back());
    std::wprintf(L"  rmw.ico (%zu sizes), icon.png\n", images.size());
}

// ---- the rices --------------------------------------------------------------------------------------------

struct Recipe
{
    std::string_view id;
    std::function<void(Surface&, float)> draw;
    std::wstring picture; // under assets\ ("" = none)
    std::wstring video;   // "" = none
    UINT videoSeconds;
    CursorStyle cursors;
};

std::vector<Recipe> recipes()
{
    return {
        {"diva-stage", drawDiva, L"diva-stage.png", L"diva-stage.mp4", 12, {hex(0x2CD5C4), hex(0x0B1026), hex(0xFF4FA3)}},
        {"tokyo-midnight", drawTokyo, L"tokyo-midnight.png", L"tokyo-midnight.mp4", 6, {hex(0x1A1B26), hex(0x7AA2F7), hex(0xBB9AF7)}},
        {"sakura-dawn", drawSakura, L"sakura-dawn.png", L"", 0, {hex(0xFFFAF3), hex(0x575279), hex(0xD7827E)}},
        {"mocha-kawaii", drawMocha, L"mocha-kawaii.png", L"", 0, {hex(0xF5C2E7), hex(0x1E1E2E), hex(0xCBA6F7)}},
        {"your-rice", drawKit, L"", L"", 0, {hex(0x2CD5C4), hex(0x0B1026), hex(0xFF4FA3)}},
    };
}

int run(int argc, wchar_t** argv)
{
    if (argc < 3)
    {
        std::fputws(L"usage: rmw-rice-art <rice> <folder> [--quick]\n", stderr);
        return 2;
    }
    const std::wstring id(argv[1]);
    if (id == L"--icon")
    {
        Gpu gpu;
        saveIcon(gpu, argv[2]);
        return 0;
    }
    const fs::path assets = fs::path(argv[2]) / L"assets";
    const bool quick = argc > 3 && std::wstring_view(argv[3]) == L"--quick";
    const auto all = recipes();
    const auto recipe = std::ranges::find_if(all, [&](const Recipe& r) { return std::wstring(r.id.begin(), r.id.end()) == id; });
    if (recipe == all.end())
    {
        std::fwprintf(stderr, L"rmw-rice-art: unknown rice '%ls'\n", id.c_str());
        return 2;
    }
    fs::create_directories(assets);
    Gpu gpu;
    std::wprintf(L"%ls:\n", id.c_str());
    if (!recipe->picture.empty())
    {
        // 4K: sharp on any monitor; Windows scales it down.
        Surface still(gpu, quick ? 1280 : 3840, quick ? 720 : 2160);
        recipe->draw(still, 0.0F);
        savePng(still, assets / recipe->picture);
    }
    if (!recipe->video.empty() && !quick)
    {
        Surface frame(gpu, 1920, 1080);
        saveLoop(frame, assets / recipe->video, 30, recipe->videoSeconds, [&](float t) { recipe->draw(frame, t); });
    }
    saveCursors(gpu, assets / L"cursors", recipe->cursors);
    if (quick)
    {
        // Every cursor at 64 px on a dark and a light strip, for a look (not part of the rice).
        constexpr UINT kCell = 80;
        Surface sheet(gpu, kCell * static_cast<UINT>(kCursors.size()), kCell * 2);
        sheet.begin(hex(0x202020));
        sheet.dc()->FillRectangle({0, kCell, sheet.width(), kCell * 2.0F}, solid(sheet.dc(), hex(0xF0F0F0)).get());
        for (std::size_t i = 0; i < kCursors.size(); ++i)
        {
            for (int row = 0; row < 2; ++row)
            {
                sheet.dc()->SetTransform(D2D1::Matrix3x2F::Scale(2, 2) *
                                         D2D1::Matrix3x2F::Translation(static_cast<float>(i * kCell) + 8, static_cast<float>(row * kCell) + 8));
                drawShape(sheet, kCursors[i].shape, recipe->cursors, 0.1F);
            }
        }
        sheet.end();
        savePng(sheet, fs::path(argv[2]) / L"cursors-preview.png");
    }
    return 0;
}

} // namespace

int wmain(int argc, wchar_t** argv) // NOLINT(misc-use-internal-linkage): process entry point
{
    const auto com = wil::CoInitializeEx(COINIT_MULTITHREADED);
    THROW_IF_FAILED(MFStartup(MF_VERSION, MFSTARTUP_LITE));
    const auto stop = wil::scope_exit([] { MFShutdown(); });
    try
    {
        return run(argc, argv);
    }
    catch (const wil::ResultException& e)
    {
        std::fwprintf(stderr, L"rmw-rice-art: failed (0x%08X)\n", static_cast<unsigned>(e.GetErrorCode()));
        return 1;
    }
}
