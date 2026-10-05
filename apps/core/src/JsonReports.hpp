#pragma once

#include "rmw/domain/Diagnostics.hpp"
#include "rmw/engine/RiceEngine.hpp"
#include "rmw/ipc/JsonRpc.hpp"

#include <utility>

// How the core reports diagnostics and operation results to its clients.
namespace rmw::core
{

inline ipc::Json diagnosticsJson(const domain::Diagnostics& diagnostics)
{
    ipc::Json items = ipc::Json::array();
    for (const auto& d : diagnostics.all())
    {
        items.push_back({{"severity", d.severity == domain::Severity::Error ? "error" : "warning"},
                         {"key", d.key},
                         {"line", d.line},
                         {"column", d.column},
                         {"message", d.message}});
    }
    return items;
}

inline ipc::Json reportJson(const engine::OperationReport& report)
{
    ipc::Json skipped = ipc::Json::array();
    for (const auto& s : report.skipped)
    {
        skipped.push_back({{"module", s.module}, {"field", s.field}, {"reason", s.reason}});
    }
    return ipc::Json{{"historyId", report.historyId},
                     {"modules", report.modules},
                     {"skipped", std::move(skipped)},
                     {"warnings", report.warnings}};
}

} // namespace rmw::core
