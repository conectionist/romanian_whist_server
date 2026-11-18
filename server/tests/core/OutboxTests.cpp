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

    // Each connection is logged under a number of its own: 11 and 12 are both member 1's.
    const auto first = recorder.connect(11);
    outbox.attach(1, first);
    CHECK(outbox.isConnected(1));
    CHECK(outbox.isCurrent(1, first.get()));
    outbox.send(1, message);
    outbox.send(1, message);

    // A newer connection wins: the older is told why and closed, and its late close of its own
    // leaves the newer alone. The newer starts its own count.
    const auto second = recorder.connect(12);
    outbox.attach(1, second);
    CHECK_FALSE(outbox.isCurrent(1, first.get()));
    outbox.detach(1, first.get());
    CHECK(outbox.isConnected(1));
    outbox.send(1, message);

    const auto older = recorder.messagesTo(11);
    REQUIRE(older.size() == 3);
    CHECK(older[0]["seq"] == 1);
    CHECK(older[1]["seq"] == 2);
    CHECK(older[2]["code"] == "openElsewhere");
    CHECK(older[2]["seq"] == 3);
    CHECK(recorder.closedAfter(11) == 3u);

    const auto newer = recorder.messagesTo(12);
    REQUIRE(newer.size() == 1);
    CHECK(newer[0]["seq"] == 1);
    CHECK_FALSE(recorder.closedAfter(12));

    outbox.detach(1, second.get());
    CHECK_FALSE(outbox.isConnected(1));
}

TEST_CASE("A connection is numbered on its own, whoever it belongs to", "[outbox]")
{
    test::Recorder recorder;
    Outbox outbox;
    const auto link = outbox.open(recorder.connect(5));

    Json::Value error(Json::objectValue);
    error["type"] = "error";
    outbox.post(*link, error);

    Json::Value welcome(Json::objectValue);
    welcome["type"] = "welcome";
    REQUIRE(outbox.attach(1, link, welcome));
    outbox.send(1, error);

    // Closed: nothing more goes out, and the member is not attached again.
    outbox.close(*link);
    outbox.post(*link, error);
    outbox.send(1, error);
    CHECK_FALSE(outbox.attach(2, link, welcome));

    const auto messages = recorder.messagesTo(5);
    REQUIRE(messages.size() == 3);
    CHECK(messages[0]["seq"] == 1);
    CHECK(messages[1]["type"] == "welcome");
    CHECK(messages[1]["seq"] == 2);
    CHECK(messages[2]["seq"] == 3);
    CHECK(recorder.closedAfter(5) == 3u);
}

TEST_CASE("Once stopped, the Outbox sends and attaches nothing", "[outbox]")
{
    test::Recorder recorder;
    Outbox outbox;
    outbox.attach(1, recorder.connect(1));
    const auto link = outbox.open(recorder.connect(2));
    outbox.stop();

    Json::Value message(Json::objectValue);
    message["type"] = "error";
    outbox.send(1, message);
    outbox.post(*link, message);
    outbox.close(*link);
    CHECK_FALSE(outbox.attach(3, link, message));
    CHECK_FALSE(outbox.isConnected(1));
    CHECK(recorder.messagesTo(1).empty());
    CHECK(recorder.messagesTo(2).empty());
    CHECK_FALSE(recorder.closedAfter(2));
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

TEST_CASE("A link nothing is heard from is aborted, and its member is no longer connected", "[outbox][silent]")
{
    using std::chrono::seconds;
    test::Recorder recorder;
    Outbox outbox;

    const auto quiet = outbox.open(recorder.connect(1));
    const auto lively = outbox.open(recorder.connect(2));
    const auto stray = outbox.open(recorder.connect(3));
    outbox.attach(1, quiet, Json::Value(Json::nullValue));
    outbox.attach(2, lively, Json::Value(Json::nullValue));

    // Only the second link has been heard from since the cutoff.
    outbox.heard(*lively, Clock::now() + seconds(10));
    const std::vector<MemberId> gone = outbox.closeSilent(Clock::now() + seconds(5));

    CHECK(gone == std::vector<MemberId>{1});
    CHECK(recorder.closedAfter(1));
    CHECK_FALSE(recorder.closedAfter(2));
    CHECK_FALSE(outbox.isConnected(1));
    CHECK(outbox.isConnected(2));

    // A connection that never said hello is aborted too, though no member loses anything.
    CHECK(recorder.closedAfter(3));

    // Nothing is aborted twice.
    CHECK(outbox.closeSilent(Clock::now() + seconds(5)).empty());
}

TEST_CASE("A silent link a newer tab has replaced does not take the member away", "[outbox][silent]")
{
    using std::chrono::seconds;
    test::Recorder recorder;
    Outbox outbox;

    const auto older = outbox.open(recorder.connect(1));
    const auto newer = outbox.open(recorder.connect(2));
    outbox.attach(1, older, Json::Value(Json::nullValue));
    outbox.attach(1, newer, Json::Value(Json::nullValue));
    outbox.heard(*newer, Clock::now() + seconds(10));

    CHECK(outbox.closeSilent(Clock::now() + seconds(5)).empty());
    CHECK(outbox.isConnected(1));
    CHECK_FALSE(recorder.closedAfter(2));
}

TEST_CASE("Once stopped, the Outbox aborts nothing", "[outbox][silent]")
{
    test::Recorder recorder;
    Outbox outbox;
    const auto link = outbox.open(recorder.connect(1));
    outbox.attach(1, link, Json::Value(Json::nullValue));
    outbox.stop();
    CHECK(outbox.closeSilent(Clock::now() + std::chrono::hours(1)).empty());
    CHECK_FALSE(recorder.closedAfter(1));
}
