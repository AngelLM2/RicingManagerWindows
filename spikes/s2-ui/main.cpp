// Spike S2 — can Slint deliver the gallery: fluid, good-looking, within budget, accessible?
// Criteria (docs/04): p95 frame <= 16 ms while scrolling, warm start <= 600 ms, RAM <= 120 MB,
// Narrator can read the gallery. Otherwise -> WinUI 3.
//
//   spike_s2.exe [renderer] [rows]     renderer: software | skia-software | femtovg | skia

#include "rmw/base/Utf.hpp"
#include "rmw/platform/Console.hpp"

#include <windows.h>

#include <wil/com.h>
#include <wil/resource.h>

#include <gallery.h>
#include <ole2.h>
#include <psapi.h>
#include <uiautomation.h>

#include <algorithm>
#include <chrono>
#include <format>
#include <list>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using Clock = std::chrono::steady_clock;

namespace
{

constexpr int kTilesPerRow = 4;
constexpr std::uint32_t kThumbWidth = 208;
constexpr std::uint32_t kThumbHeight = 117;

double privateMb()
{
    PROCESS_MEMORY_COUNTERS_EX counters{};
    GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters),
                         sizeof(counters));
    return counters.PrivateUsage / 1048576.0;
}

double msSinceProcessStart()
{
    FILETIME created{}, exited{}, kernel{}, user{};
    GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user);
    FILETIME now{};
    GetSystemTimePreciseAsFileTime(&now);
    const auto toU64 = [](FILETIME ft) {
        return (static_cast<ULONGLONG>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
    };
    return (toU64(now) - toU64(created)) / 10'000.0;
}

// A thumbnail generated on demand (stands in for a decoded image).
slint::Image makeThumb(int index)
{
    slint::SharedPixelBuffer<slint::Rgba8Pixel> buffer(kThumbWidth, kThumbHeight);
    auto* px = const_cast<slint::Rgba8Pixel*>(buffer.begin());
    const auto hue = static_cast<std::uint8_t>(index * 37);
    for (std::uint32_t y = 0; y < kThumbHeight; ++y)
    {
        for (std::uint32_t x = 0; x < kThumbWidth; ++x)
        {
            *px++ = {static_cast<std::uint8_t>(hue + x), static_cast<std::uint8_t>(80 + y),
                     static_cast<std::uint8_t>(200 - hue / 2), 255};
        }
    }
    return slint::Image(buffer);
}

// Virtualized model: rows are built only when the ListView asks for them and an LRU keeps a
// bounded number in memory — the approach the real gallery will use with decoded thumbnails.
class LazyRowsModel final : public slint::Model<TileRow>
{
public:
    explicit LazyRowsModel(int rowCount, std::size_t cacheRows)
        : m_rowCount(rowCount)
        , m_cacheRows(cacheRows)
    {
    }

    std::size_t row_count() const override { return static_cast<std::size_t>(m_rowCount); }

    std::optional<TileRow> row_data(std::size_t row) const override
    {
        if (row >= row_count())
        {
            return std::nullopt;
        }
        if (auto it = m_cache.find(row); it != m_cache.end())
        {
            m_lru.remove(row);
            m_lru.push_front(row);
            return it->second;
        }
        std::vector<Tile> tiles;
        for (int i = 0; i < kTilesPerRow; ++i)
        {
            const int index = static_cast<int>(row) * kTilesPerRow + i;
            tiles.push_back({makeThumb(index), slint::SharedString(std::format("Wallpaper {}", index + 1))});
        }
        TileRow value{std::make_shared<slint::VectorModel<Tile>>(tiles)};
        m_cache[row] = value;
        m_lru.push_front(row);
        while (m_lru.size() > m_cacheRows)
        {
            m_cache.erase(m_lru.back());
            m_lru.pop_back();
        }
        ++m_built;
        return value;
    }

    std::size_t built() const { return m_built; }

private:
    int m_rowCount;
    std::size_t m_cacheRows;
    mutable std::map<std::size_t, TileRow> m_cache;
    mutable std::list<std::size_t> m_lru;
    mutable std::size_t m_built = 0;
};

struct UiaResult
{
    int elements = 0;
    int named = 0;
    int wallpaperTiles = 0;
    int navButtons = 0;
};

UiaResult inspectAccessibility(HWND hwnd)
{
    UiaResult result;
    const auto coInit = wil::CoInitializeEx(COINIT_MULTITHREADED);
    auto uia = wil::CoCreateInstance<IUIAutomation>(CLSID_CUIAutomation, CLSCTX_INPROC_SERVER);
    wil::com_ptr<IUIAutomationElement> root;
    if (FAILED(uia->ElementFromHandle(hwnd, &root)) || !root)
    {
        return result;
    }
    wil::com_ptr<IUIAutomationCondition> all;
    uia->CreateTrueCondition(&all);
    wil::com_ptr<IUIAutomationElementArray> elements;
    root->FindAll(TreeScope_Descendants, all.get(), &elements);
    if (!elements)
    {
        return result;
    }
    elements->get_Length(&result.elements);
    for (int i = 0; i < result.elements; ++i)
    {
        wil::com_ptr<IUIAutomationElement> element;
        elements->GetElement(i, &element);
        wil::unique_bstr name;
        if (element && SUCCEEDED(element->get_CurrentName(&name)) && name && SysStringLen(name.get()) > 0)
        {
            ++result.named;
            const std::wstring_view text(name.get(), SysStringLen(name.get()));
            if (text.starts_with(L"Wallpaper "))
            {
                ++result.wallpaperTiles;
            }
            if (text == L"Cursores" || text == L"Plugins" || text == L"Barra")
            {
                ++result.navButtons;
            }
        }
    }
    return result;
}

double percentile(std::vector<double> values, double p)
{
    if (values.empty())
    {
        return 0;
    }
    std::sort(values.begin(), values.end());
    return values[static_cast<std::size_t>(p * static_cast<double>(values.size() - 1))];
}

} // namespace

int wmain(int argc, wchar_t** argv)
{
    const std::wstring renderer = argc > 1 ? argv[1] : L"software";
    const int rows = argc > 2 ? std::max(10, _wtoi(argv[2])) : 125; // 125 rows x 4 = 500 thumbnails
    SetEnvironmentVariableW(L"SLINT_BACKEND", (L"winit-" + renderer).c_str());
    // Slint prints "FPS" lines to stderr; works with every renderer (the rendering notifier
    // is not supported by the software renderer).
    SetEnvironmentVariableW(L"SLINT_DEBUG_PERFORMANCE", L"refresh_lazy,console");

    auto ui = Gallery::create();
    auto model = std::make_shared<LazyRowsModel>(rows, 24);
    ui->set_rows(model);

    double firstFrameMs = 0;
    bool inScroll = false;
    Clock::time_point frameStart{};
    std::vector<double> frameMs;
    double firstTickMs = 0;
    const bool notifierSupported =
        !ui->window()
             .set_rendering_notifier([&](slint::RenderingState state, slint::GraphicsAPI) {
                 if (state == slint::RenderingState::BeforeRendering)
                 {
                     frameStart = Clock::now();
                 }
                 else if (state == slint::RenderingState::AfterRendering)
                 {
                     if (firstFrameMs == 0)
                     {
                         firstFrameMs = msSinceProcessStart();
                     }
                     if (inScroll)
                     {
                         frameMs.push_back(
                             std::chrono::duration<double, std::milli>(Clock::now() - frameStart).count());
                     }
                 }
             })
             .has_value();

    double memIdle = 0;
    double memScrolled = 0;
    UiaResult uia;
    // UI Automation calls back into this process's UI thread, so the inspection must run on
    // another thread while the UI thread keeps pumping; it asks the UI to quit when done.
    std::jthread inspector;
    int ticks = 0;
    float scroll = 0;
    float direction = -1;
    slint::Timer driver(std::chrono::milliseconds(16), [&] {
        ++ticks;
        if (ticks == 1)
        {
            firstTickMs = msSinceProcessStart(); // event loop running, window shown
        }
        if (ticks == 90) // ~1.5 s: idle after first paint
        {
            memIdle = privateMb();
            inScroll = true;
        }
        else if (ticks > 90 && ticks <= 90 + 300) // ~5 s of continuous scrolling
        {
            scroll += direction * 40.0f;
            const float maxScroll = -(rows * 160.0f - 600.0f);
            if (scroll < maxScroll || scroll > 0)
            {
                direction = -direction;
                scroll = std::clamp(scroll, maxScroll, 0.0f);
            }
            ui->set_scroll_y(scroll);
        }
        else if (ticks == 90 + 301)
        {
            inScroll = false;
            memScrolled = privateMb();
            ui->set_scroll_y(0);
        }
        else if (ticks == 90 + 340)
        {
            const HWND hwnd = ui->window().win32_hwnd();
            inspector = std::jthread([&uia, hwnd] {
                uia = inspectAccessibility(hwnd);
                slint::invoke_from_event_loop([] { slint::quit_event_loop(); });
            });
        }
        else if (ticks > 90 + 340 + 1250) // safety net: ~20 s without an answer
        {
            slint::quit_event_loop();
        }
    });

    ui->run();

    const std::string json = std::format(
        "{{\"renderer\":\"{}\",\"notifierSupported\":{},\"firstTickMs\":{:.0f},"
        "\"firstFrameMsFromProcessStart\":{:.0f},\"privateMbIdle\":{:.1f},"
        "\"privateMbAfterScroll\":{:.1f},\"frames\":{},\"frameRenderMsP50\":{:.2f},\"frameRenderMsP95\":{:."
        "2f},"
        "\"frameRenderMsMax\":{:.2f},\"rowsBuilt\":{},\"uia\":{{\"elements\":{},\"named\":{},"
        "\"wallpaperTiles\":{},"
        "\"navButtons\":{}}}}}",
        rmw::utf16To8Lossy(renderer), notifierSupported, firstTickMs, firstFrameMs, memIdle, memScrolled,
        frameMs.size(), percentile(frameMs, 0.5), percentile(frameMs, 0.95), percentile(frameMs, 1.0),
        model->built(), uia.elements, uia.named, uia.wallpaperTiles, uia.navButtons);
    rmw::platform::writeLine(rmw::platform::Stream::Out, json);
    return 0;
}
