#pragma once

#include "rmw/base/Error.hpp"
#include "rmw/package/Package.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>

// Packages from outside (docs/06 §4): unpacked by `rmw-core --import-worker`, a process of its own
// in a Job (memory cap, no children, ended with the core), into a new folder under
// <data>\imports — the same volume as rices\ and plugins\, so putting it in place is a rename.
// The core never opens the zip itself: it reads the worker's report and the files it left.
namespace rmw::core
{

struct Unpacked
{
    std::filesystem::path folder; // <data>\imports\<random>: the caller moves or removes it
    std::size_t files = 0;
    std::uint64_t bytes = 0;
};

// Runs the worker on `archive` (a .rice or .rmwp) and waits for it. On failure nothing is left.
[[nodiscard]] Result<Unpacked> unpackInWorker(const std::filesystem::path& dataDirectory,
                                              const std::filesystem::path& archive, package::Kind kind);

// Moves an unpacked rice into <data>\rices\<its name> (" 2", " 3"... when taken). Its rice.toml
// must parse. The folder it now lives in, and the rice's name.
struct PlacedRice
{
    std::filesystem::path folder;
    std::string name;
};
[[nodiscard]] Result<PlacedRice> placeRice(const std::filesystem::path& dataDirectory,
                                           const Unpacked& unpacked);

// The worker's side: rmw-core --import-worker --kind rice|plugin --archive <file> --staging <dir>
// --report <file>. Writes {"ok": true, "files", "bytes"} or {"ok": false, "error"} to the report.
[[nodiscard]] int runImportWorker(const std::wstring& kind, const std::filesystem::path& archive,
                                  const std::filesystem::path& staging, const std::filesystem::path& report);

} // namespace rmw::core
