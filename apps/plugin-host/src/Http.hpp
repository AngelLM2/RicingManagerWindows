#pragma once

#include "rmw/scripting/Runtime.hpp"

namespace rmw::pluginhost
{

// One HTTPS request for a plugin (docs/12 §6), blocking: run it on a worker thread. The runtime
// already checked the domain, the port, the headers and the rate. Here: TLS with certificate
// checks, no redirects, no cookies, no automatic Windows logon, a response of at most
// limits::kPluginNetResponseMaxBytes, and a refusal when the name led to a local address.
[[nodiscard]] scripting::HttpResponse fetch(const scripting::HttpRequest& request);

} // namespace rmw::pluginhost
