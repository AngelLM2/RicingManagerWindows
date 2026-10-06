#include "rmw/modules/LockScreen.hpp"

#include "rmw/base/Limits.hpp"
#include "rmw/base/Log.hpp"
#include "rmw/base/Utf.hpp"
#include "rmw/domain/Images.hpp"

#include <format>
#include <string>

namespace rmw::modules
{
namespace
{

using engine::Json;

constexpr std::string_view kDelivery = R"(Software\Microsoft\Windows\CurrentVersion\ContentDeliveryManager)";
constexpr std::string_view kSpotlight = "RotatingLockScreenEnabled";
constexpr std::string_view kTips = "RotatingLockScreenOverlayEnabled";
constexpr std::string_view kTipsContent = "SubscribedContent-338387Enabled";

std::string lower(std::string text)
{
    for (char& c : text)
    {
        c = (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
    }
    return text;
}

// A DWORD switch: true, false, or null when the value is absent (or not a DWORD).
Result<Json> readSwitch(ports::IRegistry& registry, std::string_view name)
{
    auto value = registry.read(kDelivery, name);
    if (!value)
    {
        return std::unexpected(std::move(value.error()));
    }
    if (!*value || (*value)->type != ports::RegistryValue::Type::Dword)
    {
        return Json(nullptr);
    }
    return Json((*value)->dword != 0);
}

Result<void> writeSwitch(ports::IRegistry& registry, std::string_view name, const Json& value)
{
    if (value.is_null())
    {
        return registry.remove(kDelivery, name);
    }
    if (!value.is_boolean())
    {
        return fail(ErrorCode::InvalidArgument, std::format("lock screen field '{}' is malformed", name));
    }
    return registry.write(kDelivery, name, ports::RegistryValue::fromDword(value.get<bool>() ? 1 : 0));
}

// The package's picture, its declared size checked (a small file can declare a huge image), in
// the store.
Result<std::string> storePicture(engine::IAssetSink& assets, const std::string& file)
{
    const std::string context = std::format("lock screen picture '{}'", file);
    auto bytes = assets.readPackageFile(file, limits::kImageMaxBytes);
    if (!bytes)
    {
        return std::unexpected(std::move(bytes.error()).withContext(context));
    }
    const auto size = domain::rasterImageSize(*bytes);
    if (!size)
    {
        return fail(ErrorCode::Unsupported, context + ": not a PNG or JPEG picture");
    }
    if (size->width == 0 || size->height == 0 || size->width > limits::kImageMaxSide ||
        size->height > limits::kImageMaxSide ||
        static_cast<std::uint64_t>(size->width) * size->height > limits::kImageMaxPixels)
    {
        return fail(ErrorCode::LimitExceeded,
                    std::format("{}: {}x{} pixels is too large", context, size->width, size->height));
    }
    auto stored = assets.fromPackage(file);
    if (!stored)
    {
        return std::unexpected(std::move(stored.error()));
    }
    const std::u8string path = stored->u8string();
    return std::string(path.begin(), path.end());
}

} // namespace

Result<Json> LockScreenLogic::desiredFromRice(const domain::Rice& rice, engine::IAssetSink& assets) const
{
    Json desired = Json::object();
    if (!rice.lockscreen)
    {
        return desired;
    }
    const domain::LockScreenConfig& config = *rice.lockscreen;
    if (!config.image.empty())
    {
        auto stored = storePicture(assets, config.image);
        if (!stored)
        {
            return std::unexpected(std::move(stored.error()));
        }
        desired["picture"] = Json{{"spotlight", false}, {"file", *stored}};
    }
    else if (config.spotlight)
    {
        desired["picture"] = Json{{"spotlight", *config.spotlight}};
    }
    if (config.tips)
    {
        desired["tips"] = *config.tips;
        desired["tips-content"] = *config.tips;
    }
    return desired;
}

Result<void> LockScreenLogic::internalize(Json& state, engine::IAssetSink& assets) const
{
    const auto picture = state.find("picture");
    if (picture == state.end() || !picture->is_object())
    {
        return {};
    }
    const std::string file = json::stringOr(*picture, "file", "");
    const std::string lowered = lower(file);
    // Windows' own pictures never go away.
    if (file.empty() || file.find('%') != std::string::npos || lowered.starts_with(R"(c:\windows\)"))
    {
        return {};
    }
    auto wide = utf8To16(file);
    if (!wide)
    {
        return {};
    }
    auto stored = assets.fromSystem(std::filesystem::path(*wide), "");
    if (!stored)
    {
        log::warn("lockscreen: cannot keep a copy of {}: {}", file, stored.error().message());
        return {};
    }
    const std::u8string path = stored->u8string();
    (*picture)["copy"] = std::string(path.begin(), path.end());
    return {};
}

bool LockScreenLogic::fieldEquivalent(std::string_view field, const Json& a, const Json& b) const
{
    if (field != "picture" || !a.is_object() || !b.is_object())
    {
        return a == b;
    }
    const Json spotlight = json::memberOr(a, "spotlight", nullptr);
    if (spotlight != json::memberOr(b, "spotlight", nullptr))
    {
        return false;
    }
    if (spotlight.is_boolean() && spotlight.get<bool>())
    {
        return true; // Spotlight's pictures change by themselves
    }
    const std::string fileA = json::stringOr(a, "file", "");
    const std::string fileB = json::stringOr(b, "file", "");
    return fileA.empty() || fileB.empty() || lower(fileA) == lower(fileB);
}

Result<engine::CaptureResult> LockScreenSystem::capture()
{
    engine::CaptureResult result;
    auto spotlight = readSwitch(m_registry, kSpotlight);
    if (!spotlight)
    {
        return std::unexpected(std::move(spotlight.error()));
    }
    Json picture{{"spotlight", *spotlight}};
    if (auto image = m_lockScreen.currentImage())
    {
        picture["file"] = *image;
    }
    else
    {
        // Without the picture, a restore still brings Spotlight back.
        log::warn("lockscreen: the picture: {}", image.error().toString());
    }
    if (spotlight->is_boolean() && spotlight->get<bool>())
    {
        result.fidelity = engine::Fidelity::Lossy;
        result.lossyReason = "Windows Spotlight picks the lock screen pictures by itself: restoring turns it "
                             "back on, and Windows shows its next picture";
    }
    result.state["picture"] = std::move(picture);
    for (const auto& [field, name] : {std::pair{"tips", kTips}, std::pair{"tips-content", kTipsContent}})
    {
        auto value = readSwitch(m_registry, name);
        if (!value)
        {
            return std::unexpected(std::move(value.error()));
        }
        result.state[field] = std::move(*value);
    }
    return result;
}

Result<void> LockScreenSystem::apply(const Json& desired)
{
    if (const auto picture = desired.find("picture"); picture != desired.end())
    {
        if (!picture->is_object())
        {
            return fail(ErrorCode::InvalidArgument, "lock screen field 'picture' is malformed");
        }
        const std::string file = json::stringOr(*picture, "file", "");
        if (!file.empty())
        {
            const std::string copy = json::stringOr(*picture, "copy", "");
            const std::string chosen = copy.empty() || m_lockScreen.exists(file) ? file : copy;
            auto spotlightNow = readSwitch(m_registry, kSpotlight);
            auto source = m_lockScreen.imageSource();
            // The same picture, Spotlight off: nothing to do. Spotlight on: the picture again
            // turns it off (Windows does that itself).
            const bool same = source && lower(*source) == lower(chosen) && spotlightNow &&
                              !(spotlightNow->is_boolean() && spotlightNow->get<bool>());
            if (!same)
            {
                if (auto set = m_lockScreen.setImage(chosen); !set)
                {
                    return set;
                }
            }
        }
        if (const auto spotlight = picture->find("spotlight"); spotlight != picture->end())
        {
            if (auto written = writeSwitch(m_registry, kSpotlight, *spotlight); !written)
            {
                return written;
            }
        }
    }
    for (const auto& [field, name] : {std::pair{"tips", kTips}, std::pair{"tips-content", kTipsContent}})
    {
        if (const auto value = desired.find(field); value != desired.end())
        {
            if (auto written = writeSwitch(m_registry, name, *value); !written)
            {
                return written;
            }
        }
    }
    return {};
}

} // namespace rmw::modules
