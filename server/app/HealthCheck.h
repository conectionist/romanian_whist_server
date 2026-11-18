#ifndef WHIST_APP_HEALTHCHECK_H
#define WHIST_APP_HEALTHCHECK_H

#include "app/ServerOptions.h"

#include <string_view>

namespace whist
{

enum class Health
{
    Ok,

    // Asked to stop, and still serving: alive.
    Draining,

    // Anything else: no answer, or one a whist server does not give.
    Bad
};

// Reads what /health answered, the whole HTTP answer as text.
Health healthOf(std::string_view answer);

// What a container runtime runs to ask whether the server is alive: GET /health at the address and port
// `options` has, 0.0.0.0 and :: mapped to the loopback. Prints ok, draining or what went wrong, and returns 0
// for a server that is ok or draining and 1 for anything else, which Docker reads as unhealthy.
int healthCheck(const ServerOptions& options);

} // namespace whist

#endif
