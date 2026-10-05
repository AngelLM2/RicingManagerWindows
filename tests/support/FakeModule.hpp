#pragma once

#include "rmw/engine/Module.hpp"

#include <functional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace rmw::test
{

using engine::Json;

// What the fake "Windows" looks like for one module: a bag of fields, plus a log of calls.
struct FakeWorld
{
    std::vector<std::string> calls; // "apply:<id>", "capture:<id>"
};

// Both halves of a module over an in-memory field map. Failures can be injected.
class FakeModule final : public engine::IModuleLogic, public engine::IModuleSystem
{
public:
    using DesiredFn = std::function<Result<Json>(const domain::Rice&, engine::IAssetSink&)>;

    FakeModule(std::string id, FakeWorld& world, Json initial = Json::object(),
               std::vector<std::string_view> dependsOn = {})
        : m_id(std::move(id))
        , m_world(world)
        , m_fields(std::move(initial))
        , m_dependsOn(std::move(dependsOn))
    {
    }

    // ---- logic
    std::string_view id() const noexcept override { return m_id; }
    std::span<const std::string_view> dependsOn() const noexcept override { return m_dependsOn; }
    Result<Json> desiredFromRice(const domain::Rice& rice, engine::IAssetSink& assets) const override
    {
        return desired ? desired(rice, assets) : Result<Json>(Json::object());
    }

    // ---- system
    Result<engine::CaptureResult> capture() override
    {
        m_world.calls.push_back("capture:" + m_id);
        if (failCapture)
        {
            return fail(ErrorCode::System, "capture failed (injected)");
        }
        return engine::CaptureResult{m_fields, lossy ? engine::Fidelity::Lossy : engine::Fidelity::Exact,
                                     lossy ? "lossy (injected)" : ""};
    }
    Result<void> apply(const Json& fields) override
    {
        m_world.calls.push_back("apply:" + m_id);
        ++applyCount;
        if (failApplyAt != 0 && applyCount == failApplyAt)
        {
            // Half-applied on purpose: one field lands before the failure.
            if (!fields.empty())
            {
                m_fields[fields.begin().key()] = fields.begin().value();
            }
            return fail(ErrorCode::System, "apply failed (injected)");
        }
        if (failEveryApplyFrom != 0 && applyCount >= failEveryApplyFrom)
        {
            return fail(ErrorCode::System, "apply keeps failing (injected)");
        }
        if (crashApplyAt != 0 && applyCount == crashApplyAt)
        {
            throw std::runtime_error("simulated crash"); // leaves the engine mid-operation
        }
        for (const auto& [key, value] : fields.items())
        {
            m_fields[key] = value;
        }
        return {};
    }

    // ---- test controls
    Json& fields() noexcept { return m_fields; }

    DesiredFn desired;
    int applyCount = 0;
    int failApplyAt = 0;        // 1-based apply call that fails (after half-applying)
    int failEveryApplyFrom = 0; // every apply from this call on fails
    int crashApplyAt = 0;       // 1-based apply call that throws
    bool failCapture = false;
    bool lossy = false;

private:
    std::string m_id;
    FakeWorld& m_world;
    Json m_fields;
    std::vector<std::string_view> m_dependsOn;
};

} // namespace rmw::test
