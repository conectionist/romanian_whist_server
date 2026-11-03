#ifndef WHIST_CORE_OUTBOX_H
#define WHIST_CORE_OUTBOX_H

#include "core/Connection.h"
#include "core/Protocol.h"

#include <json/value.h>

#include <cstdint>
#include <memory>
#include <mutex>
#include <unordered_map>

namespace whist
{

// Every message to a member goes through here, from any thread. It is the server's own record of
// who is connected, so nothing ever asks a connection whether it is still open.
class Outbox
{
public:
    // A member has one connection at a time: attaching another replaces it.
    void attach(MemberId member, std::shared_ptr<Connection> connection);

    // Only if that connection is still the member's, so a late close of an old tab is harmless.
    void detach(MemberId member, const Connection* connection);

    bool isConnected(MemberId member) const;

    // Stamps the connection's next seq and posts, in one step under that connection's lock, so
    // messages leave in seq order. A member with no connection misses the message. Never blocks.
    void send(MemberId member, Json::Value message);

private:
    struct Link
    {
        std::shared_ptr<Connection> connection;
        std::mutex mutex;
        std::uint64_t seq = 0;
    };

    mutable std::mutex mutex_;
    std::unordered_map<MemberId, std::shared_ptr<Link>> links_;
};

} // namespace whist

#endif
