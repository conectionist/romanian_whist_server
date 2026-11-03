#ifndef WHIST_CORE_OUTBOX_H
#define WHIST_CORE_OUTBOX_H

#include "core/Connection.h"
#include "core/Protocol.h"

#include <json/value.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <unordered_map>

namespace whist
{

// Every message to a connection goes through here, from any thread. It is the server's own record of
// who is connected, so nothing ever asks a connection whether it is still open.
class Outbox
{
public:
    // One connection, numbered on its own: seq counts from 1 on each connection, whoever it belongs to.
    class Link
    {
    public:
        explicit Link(std::shared_ptr<Connection> connection);

        const Connection* connection() const;

    private:
        friend class Outbox;

        const std::shared_ptr<Connection> connection_;
        std::mutex mutex_;
        std::uint64_t seq_ = 0;
        bool closed_ = false;
    };

    std::shared_ptr<Link> open(std::shared_ptr<Connection> connection);

    // Makes the link the member's, posting `first` on it before any message to the member can reach
    // it. A link the member had before is sent openElsewhere and closed: the newer tab wins. Returns
    // false, and attaches nothing, once the Outbox has stopped or the link has closed.
    bool attach(MemberId member, const std::shared_ptr<Link>& link, Json::Value first);

    // Opens a link for the connection and attaches it, with no first message.
    void attach(MemberId member, std::shared_ptr<Connection> connection);

    // Only if that connection is still the member's, so a late close of an old tab is harmless.
    void detach(MemberId member, const Connection* connection);

    bool isConnected(MemberId member) const;
    bool isCurrent(MemberId member, const Connection* connection) const;

    // Stamps the connection's next seq and posts, in one step under that link's lock, so messages
    // leave in seq order. A member with no connection misses the message. Never blocks.
    void send(MemberId member, Json::Value message);

    // To one link, whoever it belongs to now.
    void post(Link& link, Json::Value message);

    // Closes the link after what was posted on it; anything posted later is dropped.
    void close(Link& link);

    // For shutdown. Once it returns, nothing is posted or attached again, so nothing reaches a
    // connection after Drogon's loops are gone.
    void stop();

private:
    void postLocked(Link& link, Json::Value message);
    void closeLocked(Link& link);

    // Every post holds it shared; stop() holds it alone.
    mutable std::shared_mutex gate_;
    bool stopped_ = false;

    mutable std::mutex mutex_;
    std::unordered_map<MemberId, std::shared_ptr<Link>> links_;
};

} // namespace whist

#endif
