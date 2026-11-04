#ifndef WHIST_CORE_SERVICE_H
#define WHIST_CORE_SERVICE_H

#include "core/Outbox.h"
#include "core/Registry.h"

#include <chrono>
#include <string>

namespace whist
{

struct ServiceOptions
{
    RegistryOptions registry;

    // A connection with nothing heard from it for this long is closed, and its member is away.
    std::chrono::milliseconds pongTimeout{30000};

    // What welcome says the server is.
    std::string version;
    std::string commit;
};

// The server's core as one object: who is connected, every table, and what a welcome says.
class Service
{
public:
    explicit Service(ServiceOptions options = {});

    // Shuts down first.
    ~Service();

    Service(const Service&) = delete;
    Service& operator=(const Service&) = delete;

    Outbox& outbox();
    Registry& registry();
    const std::string& version() const;
    const std::string& commit() const;

    // Closes every table and joins every game thread, then stops the Outbox, so nothing is sent
    // after it returns. Call it before Drogon quits.
    void shutdown();

    // Called every second or so, on any one thread at a time. Closes the connections nothing has
    // been heard from since `now` less the pong timeout, and tells their tables they are away. Does
    // nothing once shut down.
    void reap(Clock::time_point now);

private:
    const std::string version_;
    const std::string commit_;
    const std::chrono::milliseconds pongTimeout_;

    // The Registry's tables send through the Outbox, so it is declared first and outlives them.
    Outbox outbox_;
    Registry registry_;
};

} // namespace whist

#endif
