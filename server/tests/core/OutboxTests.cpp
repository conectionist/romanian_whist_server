#include "core/Outbox.h"

#include "support/Recorder.h"

#include <catch2/catch_test_macros.hpp>

#include <thread>
#include <vector>

using namespace whist;

TEST_CASE("The Outbox numbers each connection's messages and drops them for nobody", "[outbox]")
{
    test::Recorder recorder;
    Outbox outbox;

    Json::Value message(Json::objectValue);
    message["type"] = "error";
    outbox.send(1, message);
    CHECK_FALSE(outbox.isConnected(1));

    const auto first = recorder.connect(1);
    outbox.attach(1, first);
    CHECK(outbox.isConnected(1));
    outbox.send(1, message);
    outbox.send(1, message);

    // A newer connection starts its own count, and an old one's late close leaves it alone.
    const auto second = recorder.connect(1);
    outbox.attach(1, second);
    outbox.detach(1, first.get());
    CHECK(outbox.isConnected(1));
    outbox.send(1, message);

    const auto messages = recorder.messagesTo(1);
    REQUIRE(messages.size() == 3);
    CHECK(messages[0]["seq"] == 1);
    CHECK(messages[1]["seq"] == 2);
    CHECK(messages[2]["seq"] == 1);

    outbox.detach(1, second.get());
    CHECK_FALSE(outbox.isConnected(1));
}

TEST_CASE("Messages sent from many threads leave in seq order", "[outbox]")
{
    test::Recorder recorder;
    Outbox outbox;
    outbox.attach(1, recorder.connect(1));

    std::vector<std::thread> senders;
    for(int t = 0 ; t < 4 ; t++)
    {
        senders.emplace_back([&outbox]
        {
            for(int i = 0 ; i < 200 ; i++)
            {
                Json::Value message(Json::objectValue);
                message["type"] = "error";
                outbox.send(1, message);
            }
        });
    }
    for(std::thread& sender : senders)
        sender.join();

    const auto messages = recorder.messagesTo(1);
    REQUIRE(messages.size() == 800);
    for(std::size_t i = 0 ; i < messages.size() ; i++)
        CHECK(messages[i]["seq"].asUInt64() == i + 1);
}
