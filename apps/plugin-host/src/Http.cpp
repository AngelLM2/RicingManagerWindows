#include "Http.hpp"

#include "rmw/base/Utf.hpp"
#include "rmw/base/Version.hpp"
#include "rmw/plugins/Url.hpp"

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
bool isLocal(const sockaddr* address)
{
    if (address != nullptr && address->sa_family == AF_INET)
    {
        const auto* v4 = reinterpret_cast<const sockaddr_in*>(address);
        const auto bytes = std::bit_cast<std::array<std::uint8_t, 4>>(v4->sin_addr.s_addr);
        return plugins::isLocalAddress(bytes);
    }
    if (address != nullptr && address->sa_family == AF_INET6)
    {
        const auto* v6 = reinterpret_cast<const sockaddr_in6*>(address);
        return plugins::isLocalAddress(std::span<const std::uint8_t>(v6->sin6_addr.u.Byte, 16));
    }
    return true;
}

// Every address the name has now, checked before anything is sent: a name that leads to this PC or
// its network is refused (v1.7 looked only after the request went out). Empty: fine.
std::string localProblem(const std::string& host)
{
    WSADATA data{};
    if (WSAStartup(MAKEWORD(2, 2), &data) != 0)
    {
        return "cannot start the network";
    }
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* found = nullptr;
    const int resolved = getaddrinfo(host.c_str(), "443", &hints, &found);
    std::string problem;
    if (resolved != 0 || found == nullptr)
    {
        problem = std::format("cannot find {} (error {})", host, resolved);
    }
    for (const addrinfo* entry = found; entry != nullptr && problem.empty(); entry = entry->ai_next)
    {
        if (isLocal(entry->ai_addr))
        {
            problem = std::format("{} leads to a local address: refused", host);
        }
    }
    if (found != nullptr)
    {
        freeaddrinfo(found);
    }
    WSACleanup();
    return problem;
}

} // namespace

scripting::HttpResponse fetch(const scripting::HttpRequest& request)
{
    if (const std::string problem = localProblem(request.host); !problem.empty())
    {
        return failure(problem);
    }
    // Straight to the server, never through a proxy: behind one, the address checked after the
    // connection was the proxy's (a local proxy refused everything, a public one let a local
    // target through). A network that reaches the Internet only through a proxy leaves plugins
    // offline, and the error says so.
    const std::wstring agent = utf8To16Lossy(std::format("RicingManager-PluginHost/{}", kVersion));
    const Internet session(WinHttpOpen(agent.c_str(), WINHTTP_ACCESS_TYPE_NO_PROXY, WINHTTP_NO_PROXY_NAME,
                                       WINHTTP_NO_PROXY_BYPASS, 0));
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
        const DWORD error = GetLastError();
        if (error == ERROR_WINHTTP_CANNOT_CONNECT || error == ERROR_WINHTTP_TIMEOUT ||
            error == ERROR_WINHTTP_NAME_NOT_RESOLVED)
        {
            return failure(std::format("cannot reach {} directly (error {}); plugins do not go through a proxy",
                                       request.host, error));
        }
        return failure(std::format("the request failed (error {})", error));
    }

    // The name asked again by WinHTTP may answer otherwise (a name that changes its address between
    // two lookups): the server it reached is checked too (direct: it is the server's own address).
    WINHTTP_CONNECTION_INFO info{};
    DWORD infoSize = sizeof(info);
    info.cbSize = sizeof(info);
    if (WinHttpQueryOption(handle.get(), WINHTTP_OPTION_CONNECTION_INFO, &info, &infoSize) == FALSE)
    {
        return failure(lastErrorText(std::format("checking the address of {}", request.host)));
    }
    if (isLocal(reinterpret_cast<const sockaddr*>(&info.RemoteAddress)))
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
