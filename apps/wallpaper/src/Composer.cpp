#include "Composer.hpp"

#include "GraphicsParts.hpp"

#include "rmw/base/Log.hpp"
#include "rmw/platform/WinError.hpp"
#include "rmw/video/Particles.hpp"
#include "rmw/video/Playback.hpp"

#include <d2d1_1helper.h>
#include <d2d1effects.h>
#include <wincodec.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <format>
#include <map>
#include <numbers>
#include <vector>

namespace rmw::wallpaper
{
namespace
{

using platform::hresultError;
using domain::ParticleKind;

constexpr float kParallaxRoom = 0.05F; // of the monitor, both sides together, at parallax 1
constexpr float kZoomRoom = 0.12F;     // at zoom 1
constexpr double kZoomPeriod = 40.0;   // seconds of one slow zoom in and out
constexpr double kPanPeriodX = 53.0;
constexpr double kPanPeriodY = 61.0;
constexpr float kParallaxEase = 3.0F; // how fast the picture follows the mouse (per second)
constexpr int kMaxSide = 8192;        // what every GPU we run on takes in one bitmap
constexpr float kTau = 2.0F * std::numbers::pi_v<float>;

Result<void> check(HRESULT hr, std::string_view what)
{
    if (FAILED(hr))
    {
        return std::unexpected(hresultError(hr, what));
    }
    return {};
}

D2D1_COLOR_F colorF(domain::Color color, float alpha = 1.0F) noexcept
{
    return {static_cast<float>(color.r) / 255.0F, static_cast<float>(color.g) / 255.0F,
            static_cast<float>(color.b) / 255.0F, (static_cast<float>(color.a) / 255.0F) * alpha};
}

domain::Color mix(domain::Color a, domain::Color b, float t) noexcept
{
    const auto channel = [t](std::uint8_t x, std::uint8_t y) {
        return static_cast<std::uint8_t>(std::lround((static_cast<float>(x) * (1.0F - t)) + (static_cast<float>(y) * t)));
    };
    return {channel(a.r, b.r), channel(a.g, b.g), channel(a.b, b.b), a.a};
}

constexpr domain::Color kWhite{0xFF, 0xFF, 0xFF};

bool turns(ParticleKind kind) noexcept
{
    return kind == ParticleKind::Sparkles || kind == ParticleKind::Petals || kind == ParticleKind::Notes ||
           kind == ParticleKind::Hearts;
}

// EXIF orientation (1..8) as WIC's flip and rotation (rotation first, then the flip).
WICBitmapTransformOptions transformOf(UINT orientation) noexcept
{
    switch (orientation)
    {
    case 2:
        return WICBitmapTransformFlipHorizontal;
    case 3:
        return WICBitmapTransformRotate180;
    case 4:
        return WICBitmapTransformFlipVertical;
    case 5:
        return static_cast<WICBitmapTransformOptions>(WICBitmapTransformRotate90 | WICBitmapTransformFlipHorizontal);
    case 6:
        return WICBitmapTransformRotate90;
    case 7:
        return static_cast<WICBitmapTransformOptions>(WICBitmapTransformRotate270 | WICBitmapTransformFlipHorizontal);
    case 8:
        return WICBitmapTransformRotate270;
    default:
        return WICBitmapTransformRotate0;
    }
}

UINT orientationOf(IWICBitmapFrameDecode& frame) noexcept
{
    wil::com_ptr_nothrow<IWICMetadataQueryReader> reader;
    if (FAILED(frame.GetMetadataQueryReader(reader.put())) || !reader)
    {
        return 1;
    }
    for (const wchar_t* name : {L"System.Photo.Orientation", L"/app1/ifd/{ushort=274}"})
    {
        PROPVARIANT value{};
        PropVariantInit(&value);
        const bool found = SUCCEEDED(reader->GetMetadataByName(name, &value)) && value.vt == VT_UI2;
        const UINT orientation = found ? value.uiVal : 0;
        PropVariantClear(&value);
        if (orientation >= 1 && orientation <= 8)
        {
            return orientation;
        }
    }
    return 1;
}

D2D1_BITMAP_PROPERTIES1 targetProperties(D2D1_ALPHA_MODE alpha = D2D1_ALPHA_MODE_PREMULTIPLIED) noexcept
{
    return D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET,
                                   D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, alpha));
}

} // namespace

struct Composer::Parts
{
    wil::com_ptr_nothrow<ID2D1DeviceContext> context;
    wil::com_ptr_nothrow<ID2D1Factory> factory;
    wil::com_ptr_nothrow<ID2D1Bitmap1> base; // a picture or a color, placed and changed once
    wil::com_ptr_nothrow<ID3D11Texture2D> frameTexture;
    wil::com_ptr_nothrow<ID2D1Bitmap1> frames; // a video's frames, wrapped
    wil::com_ptr_nothrow<ID2D1Effect> blur;    // a video's: on every frame
    wil::com_ptr_nothrow<ID2D1Effect> colors;
    wil::com_ptr_nothrow<ID2D1RadialGradientBrush> vignette;
    std::map<ID3D11Texture2D*, wil::com_ptr_nothrow<ID2D1Bitmap1>> targets;

    struct Layer
    {
        video::ParticleField field;
        std::vector<wil::com_ptr_nothrow<ID2D1Bitmap1>> sprites; // one per variant
        wil::com_ptr_nothrow<ID2D1SolidColorBrush> brush;      // rain
        float side = 0.0F;                                     // a sprite's bitmap, pixels
    };
    std::vector<Layer> layers;

    // The effects that change a whole picture: blur, then the colors. Null when there are none.
    Result<wil::com_ptr_nothrow<ID2D1Effect>> chain(const video::Effects& effects, float unit)
    {
        wil::com_ptr_nothrow<ID2D1Effect> last;
        if (effects.blur > 0.0)
        {
            if (auto made = check(context->CreateEffect(CLSID_D2D1GaussianBlur, blur.put()), "create the blur");
                !made)
            {
                return std::unexpected(std::move(made.error()));
            }
            blur->SetValue(D2D1_GAUSSIANBLUR_PROP_STANDARD_DEVIATION, static_cast<float>(effects.blur) * unit);
            blur->SetValue(D2D1_GAUSSIANBLUR_PROP_BORDER_MODE, D2D1_BORDER_MODE_HARD);
            blur->SetValue(D2D1_GAUSSIANBLUR_PROP_OPTIMIZATION, D2D1_GAUSSIANBLUR_OPTIMIZATION_BALANCED);
            last = blur;
        }
        if (!effects.keepsColors())
        {
            if (auto made =
                    check(context->CreateEffect(CLSID_D2D1ColorMatrix, colors.put()), "create the color effect");
                !made)
            {
                return std::unexpected(std::move(made.error()));
            }
            const auto m = video::colorMatrix(effects);
            const D2D1_MATRIX_5X4_F matrix =
                D2D1::Matrix5x4F(m[0], m[1], m[2], m[3], m[4], m[5], m[6], m[7], m[8], m[9], m[10], m[11], m[12],
                                 m[13], m[14], m[15], m[16], m[17], m[18], m[19]);
            colors->SetValue(D2D1_COLORMATRIX_PROP_COLOR_MATRIX, matrix);
            colors->SetValue(D2D1_COLORMATRIX_PROP_CLAMP_OUTPUT, TRUE);
            if (last)
            {
                colors->SetInputEffect(0, last.get());
            }
            last = colors;
        }
        return last;
    }

    // The first effect of the chain (where the picture goes in).
    [[nodiscard]] ID2D1Effect* first() const noexcept { return blur ? blur.get() : colors.get(); }
};

Composer::Composer(std::shared_ptr<Graphics> graphics, RendererOptions options)
    : m_graphics(std::move(graphics))
    , m_options(std::move(options))
    , m_parts(std::make_unique<Parts>())
    , m_overscan(1.0F + (kParallaxRoom * static_cast<float>(m_options.effects.parallax)) +
                 (kZoomRoom * static_cast<float>(m_options.effects.zoom)))
    , m_frameWidth(static_cast<int>(std::ceil(static_cast<float>(m_options.width) * m_overscan)))
    , m_frameHeight(static_cast<int>(std::ceil(static_cast<float>(m_options.height) * m_overscan)))
{
}

Composer::~Composer() = default;

Result<void> Composer::build()
{
    const Graphics::Parts& g = m_graphics->parts();
    if (!g.d2dDevice)
    {
        return fail(ErrorCode::Unsupported, "Direct2D is not available on this GPU");
    }
    Parts& p = *m_parts;
    if (auto made = check(g.d2dDevice->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, p.context.put()),
                          "create a Direct2D context");
        !made)
    {
        return made;
    }
    p.context->GetFactory(p.factory.put());
    p.context->SetUnitMode(D2D1_UNIT_MODE_PIXELS);
    if (m_options.effects.vignette > 0.0)
    {
        const float strength = std::clamp(static_cast<float>(m_options.effects.vignette), 0.0F, 1.0F) * 0.9F;
        const std::array stops = {D2D1::GradientStop(0.0F, D2D1::ColorF(0, 0.0F)),
                                  D2D1::GradientStop(0.45F, D2D1::ColorF(0, 0.0F)),
                                  D2D1::GradientStop(1.0F, D2D1::ColorF(0, strength))};
        wil::com_ptr_nothrow<ID2D1GradientStopCollection> collection;
        p.context->CreateGradientStopCollection(stops.data(), static_cast<UINT32>(stops.size()), collection.put());
        const auto w = static_cast<float>(m_options.width);
        const auto h = static_cast<float>(m_options.height);
        if (collection)
        {
            p.context->CreateRadialGradientBrush(
                D2D1::RadialGradientBrushProperties(D2D1::Point2F(w / 2, h / 2), D2D1::Point2F(), w * 0.75F, h * 0.75F),
                collection.get(), p.vignette.put());
        }
    }
    if (m_options.source != video::Source::Video)
    {
        if (auto base = buildBase(); !base)
        {
            return base;
        }
    }
    else
    {
        const float unit = static_cast<float>(m_options.height) / 1080.0F;
        if (auto chain = p.chain(m_options.effects, unit); !chain)
        {
            return std::unexpected(std::move(chain.error()));
        }
    }
    return buildSprites();
}

Result<void> Composer::buildBase()
{
    Parts& p = *m_parts;
    const D2D1_SIZE_U size = D2D1::SizeU(static_cast<UINT32>(m_frameWidth), static_cast<UINT32>(m_frameHeight));
    wil::com_ptr_nothrow<ID2D1Bitmap1> placed;
    if (auto made = check(p.context->CreateBitmap(size, nullptr, 0, targetProperties(), placed.put()),
                          "make room for the picture");
        !made)
    {
        return made;
    }

    // The picture, decoded no larger than it is shown (WIC scales better than a draw does).
    wil::com_ptr_nothrow<ID2D1Bitmap1> picture;
    if (m_options.source == video::Source::Image)
    {
        wil::com_ptr_nothrow<IWICImagingFactory> wic;
        if (auto made = check(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                               IID_PPV_ARGS(wic.put())),
                              "start the image decoder");
            !made)
        {
            return made;
        }
        wil::com_ptr_nothrow<IWICBitmapDecoder> decoder;
        wil::com_ptr_nothrow<IWICBitmapFrameDecode> frame;
        if (auto opened = check(wic->CreateDecoderFromFilename(m_options.file.c_str(), nullptr, GENERIC_READ,
                                                               WICDecodeMetadataCacheOnDemand, decoder.put()),
                                "open the picture");
            !opened)
        {
            return opened;
        }
        if (auto read = check(decoder->GetFrame(0, frame.put()), "read the picture"); !read)
        {
            return read;
        }
        UINT rawWidth = 0;
        UINT rawHeight = 0;
        frame->GetSize(&rawWidth, &rawHeight);
        if (rawWidth == 0 || rawHeight == 0)
        {
            return fail(ErrorCode::InvalidData, "the picture is empty");
        }
        const WICBitmapTransformOptions transform = transformOf(orientationOf(*frame));
        const bool sideways = (transform & WICBitmapTransformRotate90) != 0; // 90 or 270
        // As shown (turned), then the size it is decoded at.
        const double shownWidth = sideways ? rawHeight : rawWidth;
        const double shownHeight = sideways ? rawWidth : rawHeight;
        const double cw = m_frameWidth;
        const double ch = m_frameHeight;
        double scaleX = 1.0;
        double scaleY = 1.0;
        switch (m_options.fit)
        {
        case domain::WallpaperFit::Fill:
            scaleX = scaleY = std::min(1.0, std::max(cw / shownWidth, ch / shownHeight));
            break;
        case domain::WallpaperFit::Fit:
            scaleX = scaleY = std::min({1.0, cw / shownWidth, ch / shownHeight});
            break;
        case domain::WallpaperFit::Stretch:
            scaleX = std::min(1.0, cw / shownWidth);
            scaleY = std::min(1.0, ch / shownHeight);
            break;
        case domain::WallpaperFit::Center:
        case domain::WallpaperFit::Tile:
            break;
        }
        const double limit = std::min(1.0, kMaxSide / std::max(shownWidth * scaleX, shownHeight * scaleY));
        const auto decodedWidth = static_cast<UINT>(std::max(1L, std::lround(shownWidth * scaleX * limit)));
        const auto decodedHeight = static_cast<UINT>(std::max(1L, std::lround(shownHeight * scaleY * limit)));
        wil::com_ptr_nothrow<IWICBitmapSource> source = frame;
        if (decodedWidth != static_cast<UINT>(shownWidth) || decodedHeight != static_cast<UINT>(shownHeight))
        {
            wil::com_ptr_nothrow<IWICBitmapScaler> scaler;
            // The scaler works before the turn: in the file's own orientation.
            if (FAILED(wic->CreateBitmapScaler(scaler.put())) ||
                FAILED(scaler->Initialize(source.get(), sideways ? decodedHeight : decodedWidth,
                                          sideways ? decodedWidth : decodedHeight,
                                          WICBitmapInterpolationModeHighQualityCubic)))
            {
                return fail(ErrorCode::System, "cannot scale the picture");
            }
            source = scaler;
        }
        if (transform != WICBitmapTransformRotate0)
        {
            wil::com_ptr_nothrow<IWICBitmapFlipRotator> rotator;
            if (FAILED(wic->CreateBitmapFlipRotator(rotator.put())) ||
                FAILED(rotator->Initialize(source.get(), transform)))
            {
                return fail(ErrorCode::System, "cannot turn the picture");
            }
            source = rotator;
        }
        wil::com_ptr_nothrow<IWICFormatConverter> converter;
        if (FAILED(wic->CreateFormatConverter(converter.put())) ||
            FAILED(converter->Initialize(source.get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone,
                                         nullptr, 0.0, WICBitmapPaletteTypeMedianCut)))
        {
            return fail(ErrorCode::Unsupported, "cannot convert the picture's colors");
        }
        if (auto made = check(p.context->CreateBitmapFromWicBitmap(converter.get(), nullptr, picture.put()),
                              "decode the picture");
            !made)
        {
            return made;
        }
        m_pictureWidth = static_cast<int>(shownWidth);
        m_pictureHeight = static_cast<int>(shownHeight);
    }

    p.context->SetTarget(placed.get());
    p.context->BeginDraw();
    p.context->Clear(colorF(m_options.source == video::Source::Color ? m_options.color : m_options.background));
    if (picture)
    {
        const D2D1_SIZE_U pictureSize = picture->GetPixelSize();
        if (m_options.fit == domain::WallpaperFit::Tile)
        {
            wil::com_ptr_nothrow<ID2D1BitmapBrush1> tiles;
            const auto properties = D2D1::BitmapBrushProperties1(D2D1_EXTEND_MODE_WRAP, D2D1_EXTEND_MODE_WRAP,
                                                                 D2D1_INTERPOLATION_MODE_NEAREST_NEIGHBOR);
            if (SUCCEEDED(p.context->CreateBitmapBrush(picture.get(), &properties, nullptr, tiles.put())))
            {
                // From the monitor's top left corner, like Windows.
                tiles->SetTransform(D2D1::Matrix3x2F::Translation(
                    static_cast<float>(m_frameWidth - m_options.width) / 2.0F,
                    static_cast<float>(m_frameHeight - m_options.height) / 2.0F));
                p.context->FillRectangle(
                    D2D1::RectF(0, 0, static_cast<float>(m_frameWidth), static_cast<float>(m_frameHeight)),
                    tiles.get());
            }
        }
        else
        {
            const auto placement =
                video::place(m_options.fit, static_cast<int>(pictureSize.width), static_cast<int>(pictureSize.height),
                             m_frameWidth, m_frameHeight);
            const auto rect = [](const video::Rect& r) {
                return D2D1::RectF(static_cast<float>(r.left), static_cast<float>(r.top), static_cast<float>(r.right),
                                   static_cast<float>(r.bottom));
            };
            const D2D1_RECT_F source = rect(placement.source);
            p.context->DrawBitmap(picture.get(), rect(placement.target), 1.0F,
                                  D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC, &source);
        }
    }
    if (auto drawn = check(p.context->EndDraw(), "place the picture"); !drawn)
    {
        p.context->SetTarget(nullptr);
        return drawn;
    }
    picture.reset();

    // Blur and colors, once: from here on the picture only moves.
    const float unit = static_cast<float>(m_options.height) / 1080.0F;
    auto chain = p.chain(m_options.effects, unit);
    if (!chain)
    {
        p.context->SetTarget(nullptr);
        return std::unexpected(std::move(chain.error()));
    }
    if (!*chain)
    {
        p.context->SetTarget(nullptr);
        p.base = std::move(placed);
        return {};
    }
    if (auto made = check(p.context->CreateBitmap(size, nullptr, 0, targetProperties(), p.base.put()),
                          "make room for the changed picture");
        !made)
    {
        p.context->SetTarget(nullptr);
        return made;
    }
    p.first()->SetInput(0, placed.get());
    p.context->SetTarget(p.base.get());
    p.context->BeginDraw();
    p.context->Clear(D2D1::ColorF(0));
    p.context->DrawImage(chain->get(), D2D1_INTERPOLATION_MODE_LINEAR);
    const HRESULT hr = p.context->EndDraw();
    p.context->SetTarget(nullptr);
    p.first()->SetInput(0, nullptr);
    p.blur.reset();
    p.colors.reset();
    return check(hr, "change the picture's colors");
}

Result<ID3D11Texture2D*> Composer::frames()
{
    Parts& p = *m_parts;
    if (p.frameTexture)
    {
        return p.frameTexture.get();
    }
    const Graphics::Parts& g = m_graphics->parts();
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = static_cast<UINT>(m_frameWidth);
    desc.Height = static_cast<UINT>(m_frameHeight);
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc = {1, 0};
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    if (auto made = check(g.device->CreateTexture2D(&desc, nullptr, p.frameTexture.put()), "make room for the frames");
        !made)
    {
        return std::unexpected(std::move(made.error()));
    }
    auto surface = p.frameTexture.try_query<IDXGISurface>();
    const auto properties = D2D1::BitmapProperties1(
        D2D1_BITMAP_OPTIONS_NONE, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE));
    if (!surface)
    {
        return fail(ErrorCode::System, "the frames have no surface");
    }
    if (auto made = check(p.context->CreateBitmapFromDxgiSurface(surface.get(), &properties, p.frames.put()),
                          "show the frames to Direct2D");
        !made)
    {
        return std::unexpected(std::move(made.error()));
    }
    if (ID2D1Effect* first = p.first(); first != nullptr)
    {
        first->SetInput(0, p.frames.get());
    }
    return p.frameTexture.get();
}

Result<void> Composer::buildSprites()
{
    Parts& p = *m_parts;
    std::uint32_t seed = 1;
    for (const auto& layer : m_options.particles)
    {
        Parts::Layer built{video::ParticleField(layer, m_options.width, m_options.height, seed), {}, {}, 0.0F};
        seed = (seed * 7919U) + 17U;
        const float largest = std::max(built.field.largest(), 1.0F);
        if (layer.kind == ParticleKind::Rain)
        {
            p.context->CreateSolidColorBrush(colorF(layer.color), built.brush.put());
            p.layers.push_back(std::move(built));
            continue;
        }
        built.side = std::clamp(std::ceil(largest * (layer.glow ? 2.6F : 1.25F)), 2.0F, 512.0F);
        const int variants = layer.kind == ParticleKind::Notes ? 3 : 1;
        for (int variant = 0; variant < variants; ++variant)
        {
            wil::com_ptr_nothrow<ID2D1Bitmap1> sprite;
            const auto side = static_cast<UINT32>(built.side);
            if (auto made = check(p.context->CreateBitmap(D2D1::SizeU(side, side), nullptr, 0, targetProperties(),
                                                          sprite.put()),
                                  "make a particle");
                !made)
            {
                return made;
            }
            p.context->SetTarget(sprite.get());
            p.context->BeginDraw();
            p.context->Clear(D2D1::ColorF(0, 0.0F));
            drawShape(layer, variant, built.side / 2.0F, largest / 2.0F);
            const HRESULT hr = p.context->EndDraw();
            p.context->SetTarget(nullptr);
            if (auto drawn = check(hr, "draw a particle"); !drawn)
            {
                return drawn;
            }
            built.sprites.push_back(std::move(sprite));
        }
        p.layers.push_back(std::move(built));
    }
    return {};
}

void Composer::drawShape(const video::Particles& layer, int variant, float c, float r)
{
    Parts& p = *m_parts;
    ID2D1DeviceContext& dc = *p.context;
    const auto solid = [&](domain::Color color, float alpha = 1.0F) {
        wil::com_ptr_nothrow<ID2D1SolidColorBrush> brush;
        dc.CreateSolidColorBrush(colorF(color, alpha), brush.put());
        return brush;
    };
    // A soft round light: `alphas` at 0, the middle and the edge.
    const auto light = [&](domain::Color color, float radius, std::array<float, 3> alphas, float middle = 0.35F) {
        const std::array stops = {D2D1::GradientStop(0.0F, colorF(color, alphas[0])),
                                  D2D1::GradientStop(middle, colorF(color, alphas[1])),
                                  D2D1::GradientStop(1.0F, colorF(color, alphas[2]))};
        wil::com_ptr_nothrow<ID2D1GradientStopCollection> collection;
        wil::com_ptr_nothrow<ID2D1RadialGradientBrush> brush;
        dc.CreateGradientStopCollection(stops.data(), static_cast<UINT32>(stops.size()), collection.put());
        if (collection)
        {
            dc.CreateRadialGradientBrush(
                D2D1::RadialGradientBrushProperties(D2D1::Point2F(c, c), D2D1::Point2F(), radius, radius),
                collection.get(), brush.put());
        }
        if (brush)
        {
            dc.FillEllipse(D2D1::Ellipse(D2D1::Point2F(c, c), radius, radius), brush.get());
        }
    };
    // A shape from points around the middle, in units of r.
    const auto path = [&](auto&& figure) {
        wil::com_ptr_nothrow<ID2D1PathGeometry> geometry;
        wil::com_ptr_nothrow<ID2D1GeometrySink> sink;
        if (FAILED(p.factory->CreatePathGeometry(geometry.put())) || FAILED(geometry->Open(sink.put())))
        {
            return geometry;
        }
        const auto at = [&](float x, float y) { return D2D1::Point2F(c + (x * r), c + (y * r)); };
        figure(*sink, at);
        sink->Close();
        return geometry;
    };
    const auto star = [&](float scale) {
        return path([scale](ID2D1GeometrySink& s, auto at) {
            const float k = 0.12F * scale;
            s.BeginFigure(at(0, -scale), D2D1_FIGURE_BEGIN_FILLED);
            s.AddQuadraticBezier(D2D1::QuadraticBezierSegment(at(k, -k), at(scale, 0)));
            s.AddQuadraticBezier(D2D1::QuadraticBezierSegment(at(k, k), at(0, scale)));
            s.AddQuadraticBezier(D2D1::QuadraticBezierSegment(at(-k, k), at(-scale, 0)));
            s.AddQuadraticBezier(D2D1::QuadraticBezierSegment(at(-k, -k), at(0, -scale)));
            s.EndFigure(D2D1_FIGURE_END_CLOSED);
        });
    };

    if (layer.glow)
    {
        light(layer.color, c, {0.55F, 0.22F, 0.0F});
    }
    const domain::Color color = layer.color;
    switch (layer.kind)
    {
    case ParticleKind::Sparkles: {
        if (auto shape = star(1.0F))
        {
            dc.FillGeometry(shape.get(), solid(mix(color, kWhite, 0.25F)).get());
        }
        dc.FillEllipse(D2D1::Ellipse(D2D1::Point2F(c, c), r * 0.18F, r * 0.18F), solid(kWhite).get());
        break;
    }
    case ParticleKind::Stars:
        light(mix(color, kWhite, 0.3F), r, {1.0F, 1.0F, 0.0F}, 0.55F);
        break;
    case ParticleKind::Snow:
        light(color, r, {1.0F, 0.9F, 0.0F}, 0.6F);
        break;
    case ParticleKind::Petals: {
        auto shape = path([](ID2D1GeometrySink& s, auto at) {
            s.BeginFigure(at(-0.12F, -1.0F), D2D1_FIGURE_BEGIN_FILLED);
            s.AddBezier(D2D1::BezierSegment(at(-0.9F, -0.7F), at(-0.6F, 0.5F), at(0, 1.0F)));
            s.AddBezier(D2D1::BezierSegment(at(0.6F, 0.5F), at(0.9F, -0.7F), at(0.12F, -1.0F)));
            s.AddLine(at(0, -0.8F));
            s.EndFigure(D2D1_FIGURE_END_CLOSED);
        });
        const std::array stops = {D2D1::GradientStop(0.0F, colorF(mix(color, kWhite, 0.45F))),
                                  D2D1::GradientStop(1.0F, colorF(color))};
        wil::com_ptr_nothrow<ID2D1GradientStopCollection> collection;
        wil::com_ptr_nothrow<ID2D1RadialGradientBrush> brush;
        dc.CreateGradientStopCollection(stops.data(), static_cast<UINT32>(stops.size()), collection.put());
        if (collection)
        {
            dc.CreateRadialGradientBrush(D2D1::RadialGradientBrushProperties(D2D1::Point2F(c, c + (r * 0.3F)),
                                                                             D2D1::Point2F(), r, r),
                                         collection.get(), brush.put());
        }
        if (shape && brush)
        {
            dc.FillGeometry(shape.get(), brush.get());
        }
        break;
    }
    case ParticleKind::Fireflies:
        dc.FillEllipse(D2D1::Ellipse(D2D1::Point2F(c, c), r * 0.6F, r * 0.6F), solid(mix(color, kWhite, 0.5F)).get());
        break;
    case ParticleKind::Magic:
        if (auto shape = star(1.0F))
        {
            dc.FillGeometry(shape.get(), solid(color, 0.9F).get());
        }
        dc.FillEllipse(D2D1::Ellipse(D2D1::Point2F(c, c), r * 0.45F, r * 0.45F), solid(mix(color, kWhite, 0.6F)).get());
        break;
    case ParticleKind::Notes: {
        const auto brush = solid(color);
        const auto head = [&](float x, float y) {
            dc.SetTransform(D2D1::Matrix3x2F::Rotation(-20.0F, D2D1::Point2F(c + (x * r), c + (y * r))));
            dc.FillEllipse(D2D1::Ellipse(D2D1::Point2F(c + (x * r), c + (y * r)), r * 0.3F, r * 0.22F), brush.get());
            dc.SetTransform(D2D1::Matrix3x2F::Identity());
        };
        const auto stem = [&](float x, float top, float bottom) {
            dc.FillRectangle(D2D1::RectF(c + (x * r), c + (top * r), c + ((x + 0.09F) * r), c + (bottom * r)),
                             brush.get());
        };
        if (variant == 1) // two notes and a beam
        {
            head(-0.45F, 0.62F);
            head(0.45F, 0.45F);
            stem(-0.24F, -0.6F, 0.6F);
            stem(0.66F, -0.78F, 0.43F);
            auto beam = path([](ID2D1GeometrySink& s, auto at) {
                s.BeginFigure(at(-0.24F, -0.6F), D2D1_FIGURE_BEGIN_FILLED);
                s.AddLine(at(0.75F, -0.78F));
                s.AddLine(at(0.75F, -0.56F));
                s.AddLine(at(-0.24F, -0.38F));
                s.EndFigure(D2D1_FIGURE_END_CLOSED);
            });
            if (beam)
            {
                dc.FillGeometry(beam.get(), brush.get());
            }
            break;
        }
        head(-0.1F, 0.6F);
        stem(0.11F, -0.85F, 0.58F);
        const int flags = variant == 2 ? 2 : 1;
        for (int i = 0; i < flags; ++i)
        {
            const float y = -0.85F + (0.3F * static_cast<float>(i));
            auto flag = path([y](ID2D1GeometrySink& s, auto at) {
                s.BeginFigure(at(0.2F, y), D2D1_FIGURE_BEGIN_FILLED);
                s.AddBezier(D2D1::BezierSegment(at(0.55F, y + 0.15F), at(0.75F, y + 0.4F), at(0.5F, y + 0.75F)));
                s.AddBezier(D2D1::BezierSegment(at(0.6F, y + 0.45F), at(0.45F, y + 0.3F), at(0.2F, y + 0.25F)));
                s.EndFigure(D2D1_FIGURE_END_CLOSED);
            });
            if (flag)
            {
                dc.FillGeometry(flag.get(), brush.get());
            }
        }
        break;
    }
    case ParticleKind::Hearts: {
        auto shape = path([](ID2D1GeometrySink& s, auto at) {
            s.BeginFigure(at(0, 0.85F), D2D1_FIGURE_BEGIN_FILLED);
            s.AddBezier(D2D1::BezierSegment(at(-1.1F, 0.05F), at(-0.6F, -0.95F), at(0, -0.35F)));
            s.AddBezier(D2D1::BezierSegment(at(0.6F, -0.95F), at(1.1F, 0.05F), at(0, 0.85F)));
            s.EndFigure(D2D1_FIGURE_END_CLOSED);
        });
        if (shape)
        {
            dc.FillGeometry(shape.get(), solid(color).get());
        }
        dc.FillEllipse(D2D1::Ellipse(D2D1::Point2F(c - (r * 0.38F), c - (r * 0.3F)), r * 0.16F, r * 0.12F),
                       solid(kWhite, 0.45F).get());
        break;
    }
    case ParticleKind::Bubbles: {
        const float stroke = std::max(1.0F, r * 0.09F);
        dc.FillEllipse(D2D1::Ellipse(D2D1::Point2F(c, c), r, r), solid(color, 0.15F).get());
        dc.DrawEllipse(D2D1::Ellipse(D2D1::Point2F(c, c), r - (stroke / 2), r - (stroke / 2)), solid(color, 0.85F).get(),
                       stroke);
        dc.FillEllipse(D2D1::Ellipse(D2D1::Point2F(c - (r * 0.35F), c - (r * 0.4F)), r * 0.22F, r * 0.14F),
                       solid(kWhite, 0.8F).get());
        break;
    }
    case ParticleKind::Rain:
        break; // lines, drawn as they are
    }
}

bool Composer::animated() const noexcept
{
    return m_options.effects.moves() ||
           std::ranges::any_of(m_parts->layers, [](const Parts::Layer& layer) { return !layer.field.still(); });
}

bool Composer::settled() const noexcept
{
    if (m_options.effects.zoom > 0.0 ||
        std::ranges::any_of(m_parts->layers, [](const Parts::Layer& layer) { return !layer.field.still(); }))
    {
        return false;
    }
    // Only the parallax is left: still once the picture caught up with the mouse (half a pixel).
    const float reach = kParallaxRoom / 2.0F * static_cast<float>(m_options.effects.parallax) *
                        static_cast<float>(m_options.width); // pixels at full lean
    const float tolerance = reach > 0.0F ? 0.5F / reach : 1.0F;
    return std::abs(m_parallaxX - m_drawnX) < tolerance && std::abs(m_parallaxY - m_drawnY) < tolerance &&
           std::abs(m_targetX - m_parallaxX) < tolerance && std::abs(m_targetY - m_parallaxY) < tolerance;
}

void Composer::advance(double seconds)
{
    m_time += seconds;
    for (auto& layer : m_parts->layers)
    {
        layer.field.step(seconds);
    }
    if (m_options.effects.parallax > 0.0)
    {
        POINT cursor{};
        if (GetCursorPos(&cursor) != FALSE)
        {
            const float halfWidth = static_cast<float>(m_options.width) / 2.0F;
            const float halfHeight = static_cast<float>(m_options.height) / 2.0F;
            m_targetX = std::clamp((static_cast<float>(cursor.x - m_options.left) - halfWidth) / halfWidth, -1.0F, 1.0F);
            m_targetY = std::clamp((static_cast<float>(cursor.y - m_options.top) - halfHeight) / halfHeight, -1.0F, 1.0F);
        }
        const float ease = std::min(1.0F, static_cast<float>(seconds) * kParallaxEase);
        m_parallaxX += (m_targetX - m_parallaxX) * ease;
        m_parallaxY += (m_targetY - m_parallaxY) * ease;
    }
}

long Composer::draw(ID3D11Texture2D* target)
{
    Parts& p = *m_parts;
    auto& bitmap = p.targets[target];
    if (!bitmap)
    {
        wil::com_ptr_nothrow<IDXGISurface> surface;
        RETURN_IF_FAILED(target->QueryInterface(IID_PPV_ARGS(surface.put())));
        const auto properties = D2D1::BitmapProperties1(
            D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE));
        RETURN_IF_FAILED(p.context->CreateBitmapFromDxgiSurface(surface.get(), &properties, bitmap.put()));
    }
    const auto w = static_cast<float>(m_options.width);
    const auto h = static_cast<float>(m_options.height);
    const auto& e = m_options.effects;

    // How large the picture is shown, and where its middle is.
    float zoom = 1.0F + (kParallaxRoom * static_cast<float>(e.parallax));
    float panX = 0.0F;
    float panY = 0.0F;
    if (e.zoom > 0.0)
    {
        const auto amplitude = static_cast<float>(kZoomRoom * e.zoom *
                                                  (0.5 - (0.5 * std::cos(kTau * m_time / kZoomPeriod))));
        zoom += amplitude;
        panX = amplitude / 2.0F * w * 0.6F * static_cast<float>(std::sin(kTau * m_time / kPanPeriodX));
        panY = amplitude / 2.0F * h * 0.6F * static_cast<float>(std::sin((kTau * m_time / kPanPeriodY) + 1.3));
    }
    // Away from the mouse, a little: depth.
    const float lean = kParallaxRoom / 2.0F * static_cast<float>(e.parallax);
    const float offsetX = panX - (m_parallaxX * lean * w);
    const float offsetY = panY - (m_parallaxY * lean * h);
    m_drawnX = m_parallaxX;
    m_drawnY = m_parallaxY;
    const float scale = zoom / m_overscan;
    const D2D1_MATRIX_3X2_F placed =
        D2D1::Matrix3x2F::Translation(-static_cast<float>(m_frameWidth) / 2.0F,
                                      -static_cast<float>(m_frameHeight) / 2.0F) *
        D2D1::Matrix3x2F::Scale(scale, scale) * D2D1::Matrix3x2F::Translation((w / 2.0F) + offsetX, (h / 2.0F) + offsetY);

    ID2D1DeviceContext& dc = *p.context;
    dc.SetTarget(bitmap.get());
    dc.BeginDraw();
    dc.Clear(colorF(m_options.background));
    dc.SetTransform(placed);
    if (p.frames)
    {
        if (ID2D1Effect* last = p.colors ? p.colors.get() : p.blur.get(); last != nullptr)
        {
            dc.DrawImage(last, D2D1_INTERPOLATION_MODE_LINEAR);
        }
        else
        {
            dc.DrawImage(p.frames.get(), D2D1_INTERPOLATION_MODE_LINEAR);
        }
    }
    else if (p.base)
    {
        dc.DrawImage(p.base.get(), D2D1_INTERPOLATION_MODE_LINEAR);
    }
    dc.SetTransform(D2D1::Matrix3x2F::Identity());
    if (p.vignette)
    {
        dc.FillRectangle(D2D1::RectF(0, 0, w, h), p.vignette.get());
    }
    for (auto& layer : p.layers)
    {
        const auto& sprites = layer.field.sprites();
        if (layer.brush)
        {
            for (const auto& s : sprites)
            {
                const float radians = s.rotation * std::numbers::pi_v<float> / 180.0F;
                const float dx = std::cos(radians) * s.size / 2.0F;
                const float dy = std::sin(radians) * s.size / 2.0F;
                layer.brush->SetOpacity(s.opacity);
                dc.DrawLine(D2D1::Point2F(s.x - dx, s.y - dy), D2D1::Point2F(s.x + dx, s.y + dy), layer.brush.get(),
                            std::max(1.0F, s.size / 24.0F));
            }
            continue;
        }
        if (layer.sprites.empty())
        {
            continue;
        }
        const float largest = std::max(layer.field.largest(), 1.0F);
        const bool turning = turns(layer.field.layer().kind);
        for (const auto& s : sprites)
        {
            if (s.opacity < 0.01F)
            {
                continue;
            }
            const float half = layer.side * (s.size / largest) / 2.0F;
            if (turning)
            {
                dc.SetTransform(D2D1::Matrix3x2F::Rotation(s.rotation, D2D1::Point2F(s.x, s.y)));
            }
            dc.DrawBitmap(layer.sprites[s.variant % layer.sprites.size()].get(),
                          D2D1::RectF(s.x - half, s.y - half, s.x + half, s.y + half), s.opacity,
                          D2D1_INTERPOLATION_MODE_LINEAR);
            if (turning)
            {
                dc.SetTransform(D2D1::Matrix3x2F::Identity());
            }
        }
    }
    const HRESULT hr = dc.EndDraw();
    dc.SetTarget(nullptr);
    return hr;
}

std::string Composer::describe() const
{
    std::string what;
    switch (m_options.source)
    {
    case video::Source::Image:
        what = std::format("picture {}x{}", m_pictureWidth, m_pictureHeight);
        break;
    case video::Source::Color:
        what = std::format("color {}", m_options.color.toHex());
        break;
    default:
        what = "video";
        break;
    }
    video::Effects rest = m_options.effects;
    rest.speed = 1.0;
    if (!rest.none())
    {
        what += ", effects";
    }
    if (!m_options.particles.empty())
    {
        what += std::format(", {} particle layer{}", m_options.particles.size(),
                            m_options.particles.size() == 1 ? "" : "s");
    }
    return what;
}

} // namespace rmw::wallpaper
