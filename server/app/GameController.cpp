#include "app/GameController.h"

#include "core/Connection.h"
#include "core/Session.h"

#include <trantor/net/EventLoop.h>

#include <stdexcept>
#include <utility>

namespace whist
{

namespace
{

// A Drogon connection as the core sees it. It holds the connection weakly, so a session kept in
// that connection's context makes no cycle, and nothing of ours keeps it alive.
class WsConnection : public Connection
{
public:
    WsConnection(const drogon::WebSocketConnectionPtr& connection, trantor::EventLoop* loop)
        : connection_(connection), loop_(loop)
    {
    }

    // Always queued, even on the connection's own loop, where a send would go out at once, ahead of
    // the sends other threads have queued.
    void post(std::string text) override
    {
        loop_->queueInLoop([connection = connection_, text = std::move(text)]
        {
            if(const auto live = connection.lock())
                live->send(text);
        });
    }

    void close() override
    {
        loop_->queueInLoop([connection = connection_]
        {
            if(const auto live = connection.lock())
                live->shutdown();
        });
    }

private:
    const std::weak_ptr<drogon::WebSocketConnection> connection_;
    trantor::EventLoop* const loop_;
};

} // namespace

GameController::GameController(std::shared_ptr<Service> service, std::chrono::milliseconds pingInterval)
    : service_(std::move(service)), pingInterval_(pingInterval)
{
}

void GameController::handleNewConnection(const drogon::HttpRequestPtr&,
                                         const drogon::WebSocketConnectionPtr& connection)
{
    // Drogon calls this on the connection's own I/O loop.
    trantor::EventLoop* loop = trantor::EventLoop::getEventLoopOfCurrentThread();
    if(loop == nullptr)
        throw std::logic_error("a WebSocket connection opened off its I/O loop");

    connection->setPingMessage("", pingInterval_);
    connection->setContext(std::make_shared<Session>(service_, std::make_shared<WsConnection>(connection, loop)));
}

void GameController::handleNewMessage(const drogon::WebSocketConnectionPtr& connection,
                                      std::string&& message,
                                      const drogon::WebSocketMessageType& type)
{
    const auto session = connection->getContext<Session>();
    if(!session)
        return;

    // Pings, pongs and closes are Drogon's business.
    if(type == drogon::WebSocketMessageType::Text)
        session->receive(message);
    else if(type == drogon::WebSocketMessageType::Binary)
        session->receiveBinary();
}

void GameController::handleConnectionClosed(const drogon::WebSocketConnectionPtr& connection)
{
    if(const auto session = connection->getContext<Session>())
        session->closed();
}

} // namespace whist
