#pragma once

#include "rmw/base/Log.hpp"

#include <memory>
#include <string_view>

// Unicode-correct console output (docs/02 C7): WriteConsoleW when attached to a console,
// raw UTF-8 bytes when redirected to a file or pipe. Never uses std::cout/std::wcout.
namespace rmw::platform
{

enum class Stream
{
    Out,
    Err,
};

// Writes UTF-8 text followed by a newline. Returns false if nothing could be written.
bool writeLine(Stream stream, std::string_view utf8) noexcept;

// Log sink that writes to stderr (for the CLI).
std::unique_ptr<log::ISink> makeConsoleSink();

} // namespace rmw::platform
