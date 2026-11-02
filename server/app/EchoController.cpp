#include "app/EchoController.h"

namespace whist
{

EchoController::EchoController(std::chrono::milliseconds pingInterval)
    : pingInterval_(pingInterval)
{
}

void EchoController::handleNewConnection(const drogon::HttpRequestPtr&,
                                         const drogon::WebSocketConnectionPtr& connection)
{
    connection->setPingMessage("", pingInterval_);
}

void EchoController::handleNewMessage(const drogon::WebSocketConnectionPtr& connection,
                                      std::string&& message,
                                      const drogon::WebSocketMessageType& type)
{
    if(type == drogon::WebSocketMessageType::Text)
        connection->send(message);
}

void EchoController::handleConnectionClosed(const drogon::WebSocketConnectionPtr&)
{
}

} // namespace whist
