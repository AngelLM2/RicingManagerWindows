#pragma once

#include "rmw/base/Error.hpp"
#include "rmw/domain/Rice.hpp"
#include "rmw/json/Json.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// The module contract (docs/03 §4), split by where each half runs.
//
// A module's state is a JSON object of named FIELDS ({"appsDark": true, "monitor:<key>": {...}}).
// A configuration is the same shape holding only the fields to change: what a rice asks for,
// what a snapshot restores. That makes "restore = apply" literal, lets the engine track
// lastApplied and compare three ways per field, and keeps the engine ignorant of what any
// field means.
namespace rmw::engine
{

using json::Json;

enum class Fidelity
{
    Exact,
    Lossy, // the snapshot cannot bring this back exactly (e.g. Windows Spotlight)
};

struct CaptureResult
{
    Json state = Json::object();
    Fidelity fidelity = Fidelity::Exact;
    std::string lossyReason;
};

// Copies files a configuration or snapshot refers to into the content store and returns the
// path to use instead (docs/03 §5.3): the Windows settings then point at the store, never at a
// rice folder that may be deleted.
class IAssetSink
{
public:
    virtual ~IAssetSink() = default;
    // A path inside the rice package (already checked by PathRules).
    [[nodiscard]] virtual Result<std::filesystem::path> fromPackage(std::string_view relativePath) = 0;
    // The bytes of a package file, to validate it (or read install.inf). Same containment rules.
    [[nodiscard]] virtual Result<std::vector<std::byte>> readPackageFile(std::string_view relativePath,
                                                                         std::uint64_t maxBytes) = 0;
    // A file the system uses right now (e.g. the current wallpaper), for snapshots. `extension`
    // (without dot) names the type when the file has none; empty keeps the file's own.
    [[nodiscard]] virtual Result<std::filesystem::path> fromSystem(const std::filesystem::path& file,
                                                                   std::string_view extension) = 0;
};

// The half that never touches the system. Runs in the core; must be cheap and deterministic.
class IModuleLogic
{
public:
    virtual ~IModuleLogic() = default;

    [[nodiscard]] virtual std::string_view id() const noexcept = 0;
    // Modules that must be applied first (e.g. appearance before wallpaper and cursors).
    [[nodiscard]] virtual std::span<const std::string_view> dependsOn() const noexcept { return {}; }

    // The fields this rice asks for; an empty object when the rice does not manage the module.
    // Validates beyond the parser (files exist, formats) and stores the assets it references.
    [[nodiscard]] virtual Result<Json> desiredFromRice(const domain::Rice& rice,
                                                       IAssetSink& assets) const = 0;

    // Rewrites file references in a freshly captured state so they point at the store.
    [[nodiscard]] virtual Result<void> internalize(Json& state, IAssetSink& assets) const
    {
        (void)state;
        (void)assets;
        return {};
    }

    // Whether two values of `field` mean the same (ignoring what Windows derives on its own).
    [[nodiscard]] virtual bool fieldEquivalent(std::string_view field, const Json& a, const Json& b) const
    {
        (void)field;
        return a == b;
    }
};

// The half that talks to Windows. In production it runs inside `rmw-core --apply-helper`
// (a killable process with a timeout); in tests, in-process against fakes.
class IModuleSystem
{
public:
    virtual ~IModuleSystem() = default;

    [[nodiscard]] virtual std::string_view id() const noexcept = 0;
    [[nodiscard]] virtual Result<CaptureResult> capture() = 0;
    // Sets exactly the fields in `desired`; fields it does not mention are left alone.
    // Must be idempotent: applying the same fields twice changes nothing the second time.
    [[nodiscard]] virtual Result<void> apply(const Json& desired) = 0;
};

// How the engine reaches the system halves.
class IModuleRunner
{
public:
    virtual ~IModuleRunner() = default;
    [[nodiscard]] virtual Result<CaptureResult> capture(std::string_view moduleId) = 0;
    [[nodiscard]] virtual Result<void> apply(std::string_view moduleId, const Json& desired) = 0;
};

} // namespace rmw::engine
