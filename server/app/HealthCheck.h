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

// What a container runtime runs to ask whether the server is alive: a GET /health to the address
// and port `options` has the server listen on, which a server on any address is asked at the
// loopback. Prints ok, draining or what went wrong, and returns the exit status: 0 for a server
// that is ok or draining, 1 for anything else.
int healthCheck(const ServerOptions& options);

} // namespace whist

#endif
