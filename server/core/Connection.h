#ifndef WHIST_CORE_CONNECTION_H
#define WHIST_CORE_CONNECTION_H

#include <string>

namespace whist
{

// One client's connection, as the core sees it. Both calls come from any thread, under the
// Outbox's lock for this connection, so neither may send on the calling thread or call back into
// the core.
class Connection
{
public:
    virtual ~Connection() = default;

    // Queues one message and returns.
    virtual void post(std::string text) = 0;

    // Queues a close after the messages already posted, and returns.
    virtual void close() = 0;
};

} // namespace whist

#endif
