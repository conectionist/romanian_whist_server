#ifndef WHIST_CORE_CONNECTION_H
#define WHIST_CORE_CONNECTION_H

#include <string>

namespace whist
{

// One client's connection, as the core sees it.
class Connection
{
public:
    virtual ~Connection() = default;

    // Queues one message and returns. Called from any thread, under the Outbox's lock for this
    // connection, so it must never send on the calling thread or call back into the core.
    virtual void post(std::string text) = 0;
};

} // namespace whist

#endif
