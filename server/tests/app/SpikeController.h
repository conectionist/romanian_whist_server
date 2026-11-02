#ifndef WHIST_TESTS_SPIKECONTROLLER_H
#define WHIST_TESTS_SPIKECONTROLLER_H

#include <drogon/WebSocketController.h>

#include <atomic>
#include <mutex>
#include <thread>
#include <vector>

namespace whist::test
{

// A WebSocket at /spike, for checking what the server's design takes for granted about Drogon.
// Text it does not recognise is echoed. "burst" starts two threads, neither of them Drogon's, that
// each send 500 numbered messages at once. "late" starts one that keeps sending for a while,
// so that it is still sending after the client has gone.
class SpikeController : public drogon::WebSocketController<SpikeController, false>
{
public:
    void handleNewConnection(const drogon::HttpRequestPtr& request,
                             const drogon::WebSocketConnectionPtr& connection) override;
    void handleNewMessage(const drogon::WebSocketConnectionPtr& connection,
                          std::string&& message,
                          const drogon::WebSocketMessageType& type) override;
    void handleConnectionClosed(const drogon::WebSocketConnectionPtr& connection) override;

    void reset();
    void joinSenders();

    int pongs() const { return pongs_.load(); }
    int closedConnections() const { return closed_.load(); }
    int sendsAfterClose() const { return sendsAfterClose_.load(); }

    WS_PATH_LIST_BEGIN
    WS_PATH_ADD("/spike");
    WS_PATH_LIST_END

private:
    std::atomic<int> pongs_{0};
    std::atomic<int> closed_{0};
    std::atomic<int> sendsAfterClose_{0};

    std::mutex mutex_;
    std::vector<std::jthread> senders_;
};

SpikeController& spike();

} // namespace whist::test

#endif
