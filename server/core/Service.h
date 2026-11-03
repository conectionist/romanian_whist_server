#ifndef WHIST_CORE_SERVICE_H
#define WHIST_CORE_SERVICE_H

#include "core/Outbox.h"
#include "core/Registry.h"

#include <string>

namespace whist
{

struct ServiceOptions
{
    RegistryOptions registry;

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

private:
    const std::string version_;
    const std::string commit_;

    // The Registry's tables send through the Outbox, so it is declared first and outlives them.
    Outbox outbox_;
    Registry registry_;
};

} // namespace whist

#endif
