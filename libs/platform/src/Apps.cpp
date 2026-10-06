#include "rmw/platform/Apps.hpp"

#include "rmw/base/Log.hpp"
#include "rmw/base/Utf.hpp"
#include "rmw/platform/WinError.hpp"

#include <windows.h>

#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <wil/com.h>
#include <wil/resource.h>

#include <objbase.h>

#include <algorithm>
#include <format>
#include <thread>

namespace rmw::platform
{
namespace
{

constexpr std::size_t kMaxApps = 4'096;
constexpr std::size_t kMaxIdChars = 1'024;

// The shell wants a single-threaded apartment: the work runs on a thread of its own, whatever the
// caller's apartment is.
template <class Work>
auto onShellThread(Work work) -> decltype(work())
{
    decltype(work()) result = fail(ErrorCode::System, "the shell did not answer");
    std::thread([&] {
        const auto com = wil::CoInitializeEx_failfast(COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
        result = work();
    }).join();
    return result;
}

Result<std::vector<InstalledApp>> enumerate()
{
    wil::com_ptr_nothrow<IShellItem> folder;
    if (const HRESULT hr =
            SHGetKnownFolderItem(FOLDERID_AppsFolder, KF_FLAG_DEFAULT, nullptr, IID_PPV_ARGS(folder.put()));
        FAILED(hr))
    {
        return std::unexpected(hresultError(hr, "open the list of apps"));
    }
    wil::com_ptr_nothrow<IEnumShellItems> items;
    if (const HRESULT hr = folder->BindToHandler(nullptr, BHID_EnumItems, IID_PPV_ARGS(items.put()));
        FAILED(hr))
    {
        return std::unexpected(hresultError(hr, "list the apps"));
    }
    std::vector<InstalledApp> apps;
    for (wil::com_ptr_nothrow<IShellItem> item;
         items->Next(1, item.put(), nullptr) == S_OK && apps.size() < kMaxApps;)
    {
        wil::unique_cotaskmem_string id;
        wil::unique_cotaskmem_string name;
        if (FAILED(item->GetDisplayName(SIGDN_PARENTRELATIVEPARSING, id.put())) ||
            FAILED(item->GetDisplayName(SIGDN_NORMALDISPLAY, name.put())) || id == nullptr || name == nullptr)
        {
            continue;
        }
        const std::wstring_view text(id.get());
        if (text.empty() || text.size() > kMaxIdChars)
        {
            continue;
        }
        apps.push_back({utf16To8Lossy(text), utf16To8Lossy(name.get())});
    }
    std::ranges::sort(apps, [](const InstalledApp& a, const InstalledApp& b) {
        return std::ranges::lexicographical_compare(a.name, b.name, [](char x, char y) {
            const auto lower = [](char c) {
                return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
            };
            return lower(x) < lower(y);
        });
    });
    return apps;
}

} // namespace

Result<std::vector<InstalledApp>> installedApps()
{
    return onShellThread([] { return enumerate(); });
}

Result<void> launchApp(std::string_view id)
{
    return onShellThread([id]() -> Result<void> {
        auto apps = enumerate();
        if (!apps)
        {
            return std::unexpected(std::move(apps.error()));
        }
        if (std::ranges::none_of(*apps, [id](const InstalledApp& app) { return app.id == id; }))
        {
            return fail(ErrorCode::NotFound, std::format("no installed app has the id '{}'", id));
        }
        const std::wstring target = L"shell:AppsFolder\\" + utf8To16Lossy(id);
        SHELLEXECUTEINFOW info{};
        info.cbSize = sizeof(info);
        info.fMask = SEE_MASK_FLAG_NO_UI | SEE_MASK_NOASYNC;
        info.lpVerb = L"open";
        info.lpFile = target.c_str();
        info.nShow = SW_SHOWNORMAL;
        if (ShellExecuteExW(&info) == FALSE)
        {
            return std::unexpected(win32Error(GetLastError(), std::format("open {}", id)));
        }
        return {};
    });
}

void launchAppInBackground(std::string id, std::string who)
{
    try
    {
        std::thread([id = std::move(id), who = std::move(who)] {
            try
            {
                if (auto launched = launchApp(id); !launched)
                {
                    log::warn("{}: {}", who, launched.error().toString());
                }
            }
            catch (const std::exception& e)
            {
                log::warn("{}: open {}: {}", who, id, e.what());
            }
        }).detach();
    }
    catch (const std::system_error& e)
    {
        log::warn("{}: open {}: {}", who, id, e.what());
    }
}

} // namespace rmw::platform
