#include "rmw/platform/Console.hpp"

#include "rmw/base/Utf.hpp"

#include <windows.h>

#include <algorithm>

namespace rmw::platform
{
namespace
{

bool writeAll(HANDLE handle, std::string_view utf8) noexcept
{
    DWORD mode = 0;
    if (GetConsoleMode(handle, &mode) != FALSE)
    {
        try
        {
            const std::wstring wide = utf8To16Lossy(utf8);
            std::size_t offset = 0;
            while (offset < wide.size())
            {
                const auto chunk =
                    static_cast<DWORD>(std::min<std::size_t>(wide.size() - offset, 16uz * 1024));
                DWORD written = 0;
                if (WriteConsoleW(handle, wide.data() + offset, chunk, &written, nullptr) == FALSE ||
                    written == 0)
                {
                    return false;
                }
                offset += written;
            }
            return true;
        }
        catch (...)
        {
            return false;
        }
    }

    std::size_t offset = 0;
    while (offset < utf8.size())
    {
        const auto chunk = static_cast<DWORD>(std::min<std::size_t>(utf8.size() - offset, 64uz * 1024));
        DWORD written = 0;
        if (WriteFile(handle, utf8.data() + offset, chunk, &written, nullptr) == FALSE || written == 0)
        {
            return false;
        }
        offset += written;
    }
    return true;
}

class ConsoleSink final : public log::ISink
{
public:
    void write(log::Level /*level*/, std::string_view line) noexcept override
    {
        writeLine(Stream::Err, line);
    }
};

} // namespace

bool writeLine(Stream stream, std::string_view utf8) noexcept
{
    const auto handle = GetStdHandle(stream == Stream::Out ? STD_OUTPUT_HANDLE : STD_ERROR_HANDLE);
    if (handle == nullptr || handle == INVALID_HANDLE_VALUE)
    {
        return false;
    }
    return writeAll(handle, utf8) && writeAll(handle, "\n");
}

std::unique_ptr<log::ISink> makeConsoleSink()
{
    return std::make_unique<ConsoleSink>();
}

} // namespace rmw::platform
