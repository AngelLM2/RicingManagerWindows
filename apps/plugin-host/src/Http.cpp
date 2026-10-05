#include "Http.hpp"

#include "rmw/base/Utf.hpp"
#include "rmw/base/Version.hpp"

// clang-format off
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
// clang-format on

#include <winhttp.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cstring>
#include <format>
#include <memory>

namespace rmw::pluginhost
{
namespace
{

struct InternetCloser
{
    void operator()(HINTERNET handle) const noexcept { WinHttpCloseHandle(handle); }
};
using Internet = std::unique_ptr<void, InternetCloser>;

scripting::HttpResponse failure(std::string error)
{
    scripting::HttpResponse response;
    response.error = std::move(error);
    return response;
}

std::string lastErrorText(std::string_view what)
{
    return std::format("{} failed (error {})", what, GetLastError());
}

// Loopback, private, link-local, carrier-grade NAT and the like: not the Internet.
bool isLocal(const sockaddr_storage& address)
{
    if (address.ss_family == AF_INET)
    {
        const auto& v4 = reinterpret_cast<const sockaddr_in&>(address);
        const auto b = std::bit_cast<std::array<std::uint8_t, 4>>(v4.sin_addr.s_addr);
        return b[0] == 0 || b[0] == 10 || b[0] == 127 || (b[0] == 169 && b[1] == 254) ||
               (b[0] == 172 && (b[1] & 0xF0) == 16) || (b[0] == 192 && b[1] == 168) ||
               (b[0] == 100 && (b[1] & 0xC0) == 64) || b[0] >= 224;
    }
    if (address.ss_family == AF_INET6)
    {
        const auto& v6 = reinterpret_cast<const sockaddr_in6&>(address);
        const auto* b = v6.sin6_addr.u.Byte;
        const bool loopback = std::all_of(b, b + 15, [](std::uint8_t x) { return x == 0; }) && b[15] == 1;
        const bool mapped =
            std::all_of(b, b + 10, [](std::uint8_t x) { return x == 0; }) && b[10] == 0xFF && b[11] == 0xFF;
        if (mapped)
        {
            sockaddr_storage v4{};
            auto& inner = reinterpret_cast<sockaddr_in&>(v4);
            inner.sin_family = AF_INET;
            std::memcpy(&inner.sin_addr, b + 12, 4);
            return isLocal(v4);
        }
        return loopback || (b[0] & 0xFE) == 0xFC /* unique local */ ||
               (b[0] == 0xFE && (b[1] & 0xC0) == 0x80) || b[0] == 0xFF;
    }
    return true;
}

} // namespace

scripting::HttpResponse fetch(const scripting::HttpRequest& request)
{
    const std::wstring agent = utf8To16Lossy(std::format("RicingManager-PluginHost/{}", kVersion));
    const Internet session(WinHttpOpen(agent.c_str(), WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                       WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
    if (!session)
    {
        return failure(lastErrorText("WinHttpOpen"));
    }
    WinHttpSetTimeouts(session.get(), 5'000, 10'000, 10'000, 15'000);
    DWORD protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2 | WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3;
    WinHttpSetOption(session.get(), WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols, sizeof(protocols));

    const std::wstring host = utf8To16Lossy(request.host);
    const Internet connection(WinHttpConnect(session.get(), host.c_str(), INTERNET_DEFAULT_HTTPS_PORT, 0));
    if (!connection)
    {
        return failure(lastErrorText("WinHttpConnect"));
    }
    const std::wstring method = utf8To16Lossy(request.method);
    const std::wstring target = utf8To16Lossy(request.target);
    const Internet handle(WinHttpOpenRequest(connection.get(), method.c_str(), target.c_str(), nullptr,
                                             WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                             WINHTTP_FLAG_SECURE));
    if (!handle)
    {
        return failure(lastErrorText("WinHttpOpenRequest"));
    }
    // No redirects (they could leave the declared domain), no cookies, no Windows credentials.
    DWORD redirects = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
    WinHttpSetOption(handle.get(), WINHTTP_OPTION_REDIRECT_POLICY, &redirects, sizeof(redirects));
    DWORD disable = WINHTTP_DISABLE_COOKIES | WINHTTP_DISABLE_REDIRECTS | WINHTTP_DISABLE_AUTHENTICATION;
    WinHttpSetOption(handle.get(), WINHTTP_OPTION_DISABLE_FEATURE, &disable, sizeof(disable));
    DWORD logon = WINHTTP_AUTOLOGON_SECURITY_LEVEL_HIGH;
    WinHttpSetOption(handle.get(), WINHTTP_OPTION_AUTOLOGON_POLICY, &logon, sizeof(logon));

    std::wstring headers;
    for (const auto& [name, value] : request.headers)
    {
        headers += utf8To16Lossy(std::format("{}: {}\r\n", name, value));
    }
    if (request.method == "POST" && !request.body.empty())
    {
        headers += L"Content-Type: application/json\r\n";
    }
    if (!headers.empty() &&
        WinHttpAddRequestHeaders(handle.get(), headers.c_str(), static_cast<DWORD>(headers.size()),
                                 WINHTTP_ADDREQ_FLAG_ADD | WINHTTP_ADDREQ_FLAG_REPLACE) == FALSE)
    {
        return failure(lastErrorText("WinHttpAddRequestHeaders"));
    }
    auto* body = request.body.empty() ? WINHTTP_NO_REQUEST_DATA : const_cast<char*>(request.body.data());
    const auto length = static_cast<DWORD>(request.body.size());
    if (WinHttpSendRequest(handle.get(), WINHTTP_NO_ADDITIONAL_HEADERS, 0, body, length, length, 0) ==
            FALSE ||
        WinHttpReceiveResponse(handle.get(), nullptr) == FALSE)
    {
        return failure(lastErrorText("the request"));
    }

    WINHTTP_CONNECTION_INFO info{};
    DWORD infoSize = sizeof(info);
    info.cbSize = sizeof(info);
    if (WinHttpQueryOption(handle.get(), WINHTTP_OPTION_CONNECTION_INFO, &info, &infoSize) != FALSE &&
        isLocal(info.RemoteAddress))
    {
        return failure(std::format("{} leads to a local address: refused", request.host));
    }

    scripting::HttpResponse response;
    DWORD status = 0;
    DWORD statusSize = sizeof(status);
    WinHttpQueryHeaders(handle.get(), WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX, &status, &statusSize, WINHTTP_NO_HEADER_INDEX);
    response.status = static_cast<int>(status);
    std::array<wchar_t, 256> type{};
    DWORD typeSize = sizeof(type);
    if (WinHttpQueryHeaders(handle.get(), WINHTTP_QUERY_CONTENT_TYPE, WINHTTP_HEADER_NAME_BY_INDEX,
                            type.data(), &typeSize, WINHTTP_NO_HEADER_INDEX) != FALSE)
    {
        response.contentType = utf16To8Lossy(std::wstring_view(type.data(), typeSize / sizeof(wchar_t)));
    }
    std::array<char, std::size_t{16} * 1024> buffer{};
    while (true)
    {
        DWORD read = 0;
        if (WinHttpReadData(handle.get(), buffer.data(), static_cast<DWORD>(buffer.size()), &read) == FALSE)
        {
            return failure(lastErrorText("reading the response"));
        }
        if (read == 0)
        {
            break;
        }
        if (response.body.size() + read > limits::kPluginNetResponseMaxBytes)
        {
            return failure(
                std::format("the response is larger than {} KB", limits::kPluginNetResponseMaxBytes >> 10));
        }
        response.body.append(buffer.data(), read);
    }
    return response;
}

} // namespace rmw::pluginhost
