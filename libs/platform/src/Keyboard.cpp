#include "rmw/platform/Keyboard.hpp"

#include "rmw/base/Log.hpp"

#include <windows.h>

#include <algorithm>

namespace rmw::platform
{

std::vector<KeyStroke> modifierReleases(const std::function<bool(std::uint16_t vk)>& down)
{
    std::vector<KeyStroke> strokes;
    bool winOrAlt = false;
    for (const std::uint16_t vk : kModifierKeys)
    {
        if (down(vk))
        {
            winOrAlt = winOrAlt || vk == VK_LMENU || vk == VK_RMENU || vk == VK_LWIN || vk == VK_RWIN;
            strokes.push_back({vk, true});
        }
    }
    if (winOrAlt)
    {
        strokes.insert(strokes.begin(), {{kMaskKey, false}, {kMaskKey, true}});
    }
    return strokes;
}

std::size_t releaseModifiers() noexcept
{
    try
    {
        const auto strokes =
            modifierReleases([](std::uint16_t vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; });
        if (strokes.empty())
        {
            return 0;
        }
        std::vector<INPUT> inputs;
        inputs.reserve(strokes.size());
        for (const auto& stroke : strokes)
        {
            INPUT input{};
            input.type = INPUT_KEYBOARD;
            input.ki.wVk = stroke.vk;
            input.ki.dwFlags = stroke.up ? KEYEVENTF_KEYUP : 0;
            if (stroke.vk == VK_RCONTROL || stroke.vk == VK_RMENU || stroke.vk == VK_LWIN ||
                stroke.vk == VK_RWIN)
            {
                input.ki.dwFlags |= KEYEVENTF_EXTENDEDKEY;
            }
            inputs.push_back(input);
        }
        const UINT sent = SendInput(static_cast<UINT>(inputs.size()), inputs.data(), sizeof(INPUT));
        const auto released = static_cast<std::size_t>(
            std::ranges::count_if(strokes, [](const KeyStroke& s) { return s.up && s.vk != kMaskKey; }));
        log::info("keyboard: {} modifier key(s) Windows still counted as down let go{}", released,
                  sent == inputs.size() ? "" : " (not all: the window in front may run as administrator)");
        return released;
    }
    catch (...)
    {
        return 0; // never throws: only the lists' allocation could fail
    }
}

} // namespace rmw::platform
