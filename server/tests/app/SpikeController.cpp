#include "SpikeController.h"

#include <chrono>
#include <latch>
#include <memory>
#include <string>

namespace whist::test
{

using namespace std::chrono_literals;

void SpikeController::handleNewConnection(const drogon::HttpRequestPtr&,
                                          const drogon::WebSocketConnectionPtr& connection)
{
    connection->setPingMessage("", 100ms);
}

void SpikeController::handleNewMessage(const drogon::WebSocketConnectionPtr& connection,
                                       std::string&& message,
                                       const drogon::WebSocketMessageType& type)
{
    if(type == drogon::WebSocketMessageType::Pong)
    {
        pongs_++;
        return;
    }

    if(type != drogon::WebSocketMessageType::Text)
        return;

    const std::lock_guard lock(mutex_);

    if(message == "burst")
    {
        auto start = std::make_shared<std::latch>(2);
        for(const char* name : {"a", "b"})
            senders_.emplace_back([connection, start, name]
            {
                start->arrive_and_wait();
                for(int i = 0 ; i < 500 ; i++)
                    connection->send(std::string(name) + ":" + std::to_string(i));
            });
    }
    else if(message == "late")
    {
        senders_.emplace_back([this, connection]
        {
            const auto end = std::chrono::steady_clock::now() + 1500ms;
            for(int i = 0 ; std::chrono::steady_clock::now() < end ; i++)
            {
                const bool closed = closed_.load() > 0;
                connection->send("late:" + std::to_string(i));
                if(closed)
                    sendsAfterClose_++;
                std::this_thread::sleep_for(1ms);
            }
        });
    }
    else
        connection->send(message);
}

void SpikeController::handleConnectionClosed(const drogon::WebSocketConnectionPtr&)
{
    closed_++;
}

void SpikeController::reset()
{
    joinSenders();
    pongs_ = 0;
    closed_ = 0;
    sendsAfterClose_ = 0;
}

void SpikeController::joinSenders()
{
    // The threads join as `senders` goes, outside the lock.
    std::vector<std::jthread> senders;
    {
        const std::lock_guard lock(mutex_);
        senders.swap(senders_);
    }
}

} // namespace whist::test
