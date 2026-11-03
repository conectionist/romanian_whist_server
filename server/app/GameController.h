#ifndef WHIST_APP_GAMECONTROLLER_H
#define WHIST_APP_GAMECONTROLLER_H

#include "core/Service.h"

#include <drogon/WebSocketController.h>

#include <chrono>
#include <memory>

namespace whist
{

// The game's WebSocket. Each connection gets a Session, which does everything; this only carries
// frames between Drogon and it.
class GameController : public drogon::WebSocketController<GameController, false>
{
public:
    GameController(std::shared_ptr<Service> service, std::chrono::milliseconds pingInterval);

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
    const std::shared_ptr<Service> service_;
    const std::chrono::milliseconds pingInterval_;
};

} // namespace whist

#endif
