#include "DockWidget.hpp"

#include "WindowList.hpp"

#include "rmw/base/Log.hpp"
#include "rmw/base/Utf.hpp"
#include "rmw/platform/Apps.hpp"
#include "rmw/providers/Windows.hpp"

#include <windows.h>

#include <knownfolders.h>
#include <shlobj.h>
#include <wil/resource.h>

#include <algorithm>
#include <cwctype>
#include <map>
#include <vector>

namespace rmw::barapp
{
namespace
{

using slint::interpreter::Value;

constexpr std::size_t kMaxPinned = 32;
constexpr int kDockIconSize = 64; // the icon grows under the mouse: a little more than shown

std::wstring lower(std::wstring text)
{
    std::ranges::transform(text, text.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
    return text;
}

class DockProvider final : public Provider
{
public:
    DockProvider(const bar::Slot& slot, const Instance& instance)
        : m_property(slot.prefix + "-apps")
        , m_instance(instance)
        , m_tracker(&WindowTracker::instance())
    {
        if (const auto* pinned = slot.item->config.find("pinned"))
        {
            if (const auto* list = std::get_if<domain::Value::Array>(&pinned->data))
            {
                for (const auto& entry : *list)
                {
                    const auto* id = std::get_if<std::string>(&entry.data);
                    if (id != nullptr && !id->empty() && id->size() < 512 && m_pinned.size() < kMaxPinned)
                    {
                        m_pinned.push_back({*id, comparableAppId(utf8To16Lossy(*id)), {}});
                    }
                }
            }
        }
        for (auto& app : m_pinned)
        {
            const auto pixels = providers::appIconPixels(utf8To16Lossy(app.id), kDockIconSize);
            app.icon = imageFromPixels(pixels);
        }
        instance->set_callback(slot.prefix + "-open", [this](std::span<const Value> arguments) {
            if (!arguments.empty())
            {
                if (const auto number = arguments[0].to_number();
                    number && *number >= 0 && *number < static_cast<double>(m_groups.size()))
                {
                    open(m_groups[static_cast<std::size_t>(*number)]);
                }
            }
            return Value();
        });
        m_subscription = m_tracker->subscribe([this] { update(); });
        update();
    }

    ~DockProvider() override { m_tracker->unsubscribe(m_subscription); }
    DockProvider(const DockProvider&) = delete;
    DockProvider& operator=(const DockProvider&) = delete;
    DockProvider(DockProvider&&) = delete;
    DockProvider& operator=(DockProvider&&) = delete;

private:
    struct Pinned
    {
        std::string id;      // as `rmw apps` gives it (what is opened)
        std::wstring match;  // comparableAppId
        slint::Image icon;
    };
    struct Group
    {
        std::string pinnedId; // empty: not pinned
        std::wstring identity;
        std::vector<void*> windows;
    };

    void open(const Group& group)
    {
        if (group.windows.empty())
        {
            if (auto launched = platform::launchApp(group.pinnedId); !launched)
            {
                log::warn("bar: dock: {}", launched.error().toString());
            }
            return;
        }
        const auto at = std::ranges::find(group.windows, m_tracker->foreground());
        if (at == group.windows.end() || group.windows.size() == 1)
        {
            // Not in front: forward. Its only window in front: minimized (what a taskbar does).
            WindowTracker::activate(at == group.windows.end() ? group.windows.front() : *at);
            return;
        }
        // Several windows, one in front: the next one, one click each.
        const auto next = std::next(at) == group.windows.end() ? group.windows.begin() : std::next(at);
        WindowTracker::activate(*next);
    }

    std::wstring identityOf(void* window)
    {
        if (const auto known = m_identities.find(window); known != m_identities.end())
        {
            return known->second;
        }
        return m_identities.emplace(window, lower(providers::appIdentity(window))).first->second;
    }

    void update()
    {
        std::vector<Group> groups;
        groups.reserve(m_pinned.size() + m_tracker->windows().size());
        for (const auto& app : m_pinned)
        {
            groups.push_back({app.id, app.match, {}});
        }
        std::map<void*, std::wstring> seen;
        std::map<void*, const WindowTracker::Entry*> entries;
        for (const auto& entry : m_tracker->windows())
        {
            const std::wstring identity = identityOf(entry.window);
            seen.emplace(entry.window, identity);
            entries.emplace(entry.window, &entry);
            auto group = std::ranges::find_if(groups, [&](const Group& g) {
                return !identity.empty() && g.identity == identity;
            });
            if (group == groups.end())
            {
                groups.push_back({{}, identity, {}});
                group = std::prev(groups.end());
            }
            group->windows.push_back(entry.window);
        }
        m_identities = std::move(seen); // windows that closed are forgotten

        auto model = std::make_shared<slint::VectorModel<Value>>();
        void* front = m_tracker->foreground();
        for (std::size_t i = 0; i < groups.size(); ++i)
        {
            const Group& group = groups[i];
            const WindowTracker::Entry* first = group.windows.empty() ? nullptr : entries[group.windows.front()];
            const auto pinned = std::ranges::find(m_pinned, group.pinnedId, &Pinned::id);
            slint::Image icon = pinned != m_pinned.end() && !group.pinnedId.empty() ? pinned->icon : slint::Image();
            if (first != nullptr && (icon.size().width == 0))
            {
                icon = first->icon;
            }
            const std::string title = first != nullptr ? first->title : group.pinnedId;
            const slint::interpreter::Struct item{
                {"id", Value(static_cast<double>(i))},
                {"title", Value(slint::SharedString(title))},
                {"icon", Value(icon)},
                {"running", Value(!group.windows.empty())},
                {"active", Value(std::ranges::find(group.windows, front) != group.windows.end())},
                {"count", Value(static_cast<double>(group.windows.size()))},
            };
            model->push_back(Value(item));
        }
        m_groups = std::move(groups);
        if (auto instance = m_instance.lock())
        {
            (*instance)->set_property(m_property, Value(std::shared_ptr<slint::Model<Value>>(model)));
        }
    }

    std::string m_property;
    slint::ComponentWeakHandle<slint::interpreter::ComponentInstance> m_instance;
    WindowTracker* m_tracker;
    std::vector<Pinned> m_pinned;
    std::vector<Group> m_groups; // what the shown icons are, in order
    std::map<void*, std::wstring> m_identities;
    std::size_t m_subscription = 0;
};

} // namespace

std::wstring comparableAppId(std::wstring_view id)
{
    // "{GUID}\\path\\app.exe": a known folder and a path inside it (a desktop app's id).
    if (id.size() > 39 && id.front() == L'{' && id[37] == L'}' && id[38] == L'\\')
    {
        GUID folder{};
        const std::wstring guid(id.substr(0, 38));
        if (SUCCEEDED(CLSIDFromString(guid.c_str(), &folder)))
        {
            wil::unique_cotaskmem_string path;
            if (SUCCEEDED(SHGetKnownFolderPath(folder, KF_FLAG_DEFAULT, nullptr, &path)))
            {
                return lower(std::wstring(path.get()) + std::wstring(id.substr(38)));
            }
        }
    }
    return lower(std::wstring(id));
}

std::unique_ptr<Provider> makeDockProvider(const bar::Slot& slot, const Instance& instance)
{
    return std::make_unique<DockProvider>(slot, instance);
}

} // namespace rmw::barapp
