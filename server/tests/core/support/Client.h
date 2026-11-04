#ifndef WHIST_TESTS_CORE_CLIENT_H
#define WHIST_TESTS_CORE_CLIENT_H

#include "core/Service.h"
#include "core/Session.h"

#include "support/Recorder.h"
#include "support/ScriptedHuman.h"

#include <json/value.h>

#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>

namespace whist::test
{

// One browser tab: a Session over a connection whose messages are logged under `label`. Calls are
// made one at a time, as Drogon makes them on a connection's I/O thread, whichever test thread
// makes them.
class Client
{
public:
    Client(std::shared_ptr<Service> service, Recorder& recorder, MemberId label);

    void sendText(std::string_view text);
    void send(const Json::Value& message);

    // Sends hello, and waits for the sync that always follows the welcome. Returns the welcome.
    Json::Value hello(const std::optional<std::string>& token = std::nullopt);

    // Sends hello and waits for nothing: a tab that loses its member to another gets no sync.
    void sendHello(const std::optional<std::string>& token);

    // The connection closes, as when a tab is reloaded.
    void drop();

    // The peer has sent something, a pong perhaps.
    void heard();

    MemberId label() const;
    std::optional<MemberId> member();

    ScriptedHuman::Answerer answerer();
    ScriptedHuman::Continuer continuer();

private:
    Recorder& recorder_;
    const MemberId label_;
    std::mutex mutex_;
    Session session_;
};

Json::Value typed(std::string_view type);

} // namespace whist::test

#endif
