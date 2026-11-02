#ifndef WHIST_APP_ECHOCONTROLLER_H
#define WHIST_APP_ECHOCONTROLLER_H

#include <drogon/WebSocketController.h>

#include <chrono>

namespace whist
{

// Sends every text frame straight back. The server's only WebSocket until the protocol arrives.
class EchoController : public drogon::WebSocketController<EchoController, false>
{
public:
    explicit EchoController(std::chrono::milliseconds pingInterval);

    void handleNewConnection(const drogon::HttpRequestPtr& request,
                             const drogon::WebSocketConnectionPtr& connection) override;
    void handleNewMessage(const drogon::WebSocketConnectionPtr& connection,
                          std::string&& message,
                          const drogon::WebSocketMessageType& type) override;
    void handleConnectionClosed(const drogon::WebSocketConnectionPtr& connection) override;

    WS_PATH_LIST_BEGIN
    WS_PATH_ADD("/ws");
    WS_PATH_LIST_END

private:
    std::chrono::milliseconds pingInterval_;
};

} // namespace whist

#endif
