#ifndef WHIST_CORE_SESSION_H
#define WHIST_CORE_SESSION_H

#include "core/Connection.h"
#include "core/Outbox.h"
#include "core/Protocol.h"
#include "core/Service.h"

#include <json/value.h>

#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace whist
{

// One connection's conversation with the server: hello first, then whatever the member asks of
// their table. Every call comes from the connection's own I/O thread, one at a time, so it needs no
// lock of its own.
class Session
{
public:
    // `address` is what the limits count this connection under, as clientKey() gives it; empty is
    // not limited. A connection that is one too many from its address is told so and closed.
    Session(std::shared_ptr<Service> service, std::shared_ptr<Connection> connection, std::string address = {});

    // Gives the connection's place under its address back, whether or not closed() was called.
    ~Session();

    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;

    // One text frame.
    void receive(std::string_view text);

    // A frame that is not text.
    void receiveBinary();

    // Any frame from the peer, a pong included: it is alive. Any thread.
    void heard();

    // The connection has closed.
    void closed();

    std::optional<MemberId> member() const;

    // The fifth message that does not decode closes the connection. A sound message the rules refuse
    // never does.
    static constexpr int badMessageLimit = 5;

    // A message beyond the rate within this long of the last one that was told closes the connection.
    static constexpr std::chrono::seconds excessWindow{10};

private:
    // False when the connection is over its rate. The first time it is told, and the next time
    // within the window it is closed.
    bool withinRate();

    // A table made for the member, within what their address may make. `make` makes it.
    void create(const std::function<Registry::Created()>& make);

    void giveUpAddress();

    void handle(const ClientMessage& message);
    void hello(const Hello& hello);
    void leave();

    // Errors go back on this connection, never to whichever connection the member has now.
    void reply(Json::Value message);
    void refuse(ErrorCode code, std::string_view detail = {});
    void refuse(const Refusal& refusal);
    void refuse(const std::optional<Refusal>& refusal);
    void refuseMalformed(ErrorCode code, std::string_view detail);
    void closeConnection();

    // Gives the member's connection up, and tells their table when it was still theirs.
    void detach();

    // False once a newer connection has taken this member over.
    bool current() const;

    const std::shared_ptr<Service> service_;
    const std::shared_ptr<Connection> connection_;
    const std::shared_ptr<Outbox::Link> link_;
    const std::string address_;
    TokenBucket bucket_;
    std::optional<Clock::time_point> excessAt_;
    bool counted_ = false;

    std::optional<MemberId> member_;
    std::string lang_;
    int badMessages_ = 0;
    bool closing_ = false;
};

} // namespace whist

#endif
