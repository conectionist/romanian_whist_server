#include "core/Codec.h"
#include "core/Service.h"

#include "support/Client.h"
#include "support/Games.h"
#include "support/Recorder.h"
#include "support/ServerMessages.h"

#include <catch2/catch_test_macros.hpp>

#include <latch>
#include <thread>

using namespace whist;
using std::chrono::milliseconds;

namespace
{

const milliseconds patience(60000 * test::slowdown);

std::shared_ptr<Service> quickService(milliseconds instantFloor = milliseconds(0))
{
    ServiceOptions options;
    options.registry.table.instantFloor = instantFloor;
    options.registry.table.threadNiceness = 0;
    return std::make_shared<Service>(std::move(options));
}

Json::Value createSolo(const Settings& settings)
{
    Json::Value message = test::typed("createSolo");
    message["settings"] = encodeSettings(settings);
    return message;
}

void checkShapes(const std::vector<Json::Value>& messages)
{
    for(const Json::Value& message : messages)
    {
        INFO(message.toStyledString());
        CHECK(test::shapeError(message).empty());
    }
}

} // namespace

TEST_CASE("hello comes first and once, and is answered with a welcome and a sync", "[session]")
{
    test::Recorder recorder;
    const auto service = quickService();
    test::Client client(service, recorder, 1);

    client.send(test::typed("pause"));
    const Json::Value welcome = client.hello();

    const auto messages = recorder.messagesTo(1);
    checkShapes(messages);
    REQUIRE(messages.size() >= 3);

    // Numbered by the connection, the error before hello included.
    CHECK(messages[0]["code"] == "badMessage");
    CHECK(messages[0]["detail"] == "type");
    CHECK(messages[0]["seq"] == 1);
    CHECK(welcome["seq"] == 2);
    CHECK(welcome["token"].asString().size() == 22);
    CHECK(welcome["member"].asUInt64() == *client.member());
    CHECK(messages[2]["type"] == "sync");
    CHECK(messages[2]["seq"] == 3);
    CHECK(messages[2]["table"].isNull());
    CHECK(messages[2]["game"].isNull());
    CHECK(messages[2]["prompt"].isNull());
}

TEST_CASE("A second hello is refused", "[session]")
{
    test::Recorder recorder;
    const auto service = quickService();
    test::Client client(service, recorder, 1);
    client.hello();
    client.send(test::typed("hello"));

    Json::Value again = test::typed("hello");
    again["protocol"] = protocolVersion;
    again["lang"] = "en";
    client.send(again);

    const auto messages = recorder.messagesTo(1);
    REQUIRE(messages.size() == 4);
    CHECK(messages[2]["code"] == "badMessage");
    CHECK(messages[3]["code"] == "badMessage");
    CHECK(messages[3]["detail"] == "type");
}

TEST_CASE("Another protocol version is refused, and the connection closed", "[session]")
{
    test::Recorder recorder;
    const auto service = quickService();
    test::Client client(service, recorder, 1);

    Json::Value hello = test::typed("hello");
    hello["protocol"] = protocolVersion + 1;
    hello["lang"] = "en";
    client.send(hello);
    client.send(test::typed("pause"));

    const auto messages = recorder.messagesTo(1);
    REQUIRE(messages.size() == 1);
    CHECK(messages[0]["code"] == "version");
    CHECK(recorder.closedAfter(1) == 1u);
    CHECK_FALSE(client.member());
}

TEST_CASE("A known token is the same member; an unknown one gets a new member", "[session]")
{
    test::Recorder recorder;
    const auto service = quickService();

    test::Client first(service, recorder, 1);
    const Json::Value welcome = first.hello();
    const std::string token = welcome["token"].asString();
    first.drop();

    test::Client again(service, recorder, 2);
    const Json::Value back = again.hello(token);
    CHECK(back["token"] == token);
    CHECK(back["member"] == welcome["member"]);

    // A server that restarted has forgotten every token.
    test::Client stranger(service, recorder, 3);
    const Json::Value fresh = stranger.hello(std::string(22, 'A'));
    CHECK(fresh["token"] != std::string(22, 'A'));
    CHECK(fresh["member"] != welcome["member"]);
}

TEST_CASE("The fifth bad message closes the connection", "[session]")
{
    test::Recorder recorder;
    const auto service = quickService();
    test::Client client(service, recorder, 1);
    client.hello();

    for(int i = 0 ; i < Session::badMessageLimit + 2 ; i++)
        client.sendText("not json");

    const auto messages = recorder.messagesTo(1);
    REQUIRE(messages.size() == 2 + Session::badMessageLimit);
    for(std::size_t i = 2 ; i < messages.size() ; i++)
        CHECK(messages[i]["code"] == "badMessage");
    CHECK(recorder.closedAfter(1) == messages.size());
}

TEST_CASE("Errors go back on the connection that caused them", "[session]")
{
    test::Recorder recorder;
    const auto service = quickService();
    test::Client client(service, recorder, 1);
    client.hello();

    Json::Value play = test::typed("play");
    play["prompt"] = 1;
    play["index"] = 0;
    client.send(play);
    client.send(test::typed("pause"));
    client.send(test::typed("leaveTable"));
    Json::Value join = test::typed("joinTable");
    join["code"] = "K7QM4P";
    join["profile"] = Json::Value(Json::objectValue);
    join["profile"]["name"] = "Ana";
    join["profile"]["gender"] = "female";
    join["profile"]["avatar"] = "face";
    client.send(join);

    const auto messages = recorder.messagesTo(1);
    REQUIRE(messages.size() == 6);
    CHECK(messages[2]["code"] == "notYourTurn");
    CHECK(messages[3]["code"] == "illegal");
    CHECK(messages[4]["code"] == "illegal");
    CHECK(messages[5]["code"] == "illegal");
}

TEST_CASE("A newer tab takes the member over, and the older is told why and closed", "[session]")
{
    test::Recorder recorder;
    const auto service = quickService();

    test::Client older(service, recorder, 1);
    const std::string token = older.hello()["token"].asString();

    test::Client newer(service, recorder, 2);
    newer.hello(token);

    const auto told = recorder.messagesTo(1);
    REQUIRE(told.size() == 3);
    CHECK(told[2]["code"] == "openElsewhere");
    CHECK(recorder.closedAfter(1) == 3u);

    // Whatever the older tab still sends is ignored: it starts no table and gets no answer.
    older.send(createSolo(test::tableSettings(3, {0})));
    CHECK(recorder.messagesTo(1).size() == 3);
    CHECK(service->registry().tableCount() == 0);

    newer.send(createSolo(test::tableSettings(3, {0})));
    CHECK(service->registry().tableCount() == 1);
}

TEST_CASE("Two hellos with one token at once: exactly one connection keeps the member", "[session][race]")
{
    test::Recorder recorder;
    const auto service = quickService();
    test::Client first(service, recorder, 1);
    const std::string token = first.hello()["token"].asString();
    first.drop();

    for(MemberId round = 0 ; round < 50 ; round++)
    {
        const MemberId a = 100 + 2 * round;
        const MemberId b = a + 1;
        test::Client left(service, recorder, a);
        test::Client right(service, recorder, b);

        std::latch start(2);
        std::jthread one([&] { start.arrive_and_wait(); left.sendHello(token); });
        std::jthread two([&] { start.arrive_and_wait(); right.sendHello(token); });
        one.join();
        two.join();

        INFO("round " << round);
        CHECK(recorder.closedAfter(a).has_value() != recorder.closedAfter(b).has_value());
        right.drop();
        left.drop();
    }
}

TEST_CASE("Coming back over and over during a game: each connection starts with its welcome", "[session][race]")
{
    test::Recorder recorder;
    const auto service = quickService(milliseconds(2));

    test::Client watcher(service, recorder, 1);
    const std::string token = watcher.hello()["token"].asString();
    watcher.send(createSolo(test::tableSettings(4, {})));

    std::vector<std::unique_ptr<test::Client>> tabs;
    for(MemberId i = 0 ; i < 50 ; i++)
    {
        tabs.push_back(std::make_unique<test::Client>(service, recorder, 100 + i));
        tabs.back()->hello(token);
    }

    for(MemberId label = 1 ; label < 150 ; label++)
    {
        const auto messages = recorder.messagesTo(label);
        if(messages.empty())
            continue;
        INFO("connection " << label);
        CHECK(messages[0]["type"] == "welcome");
        for(std::size_t i = 0 ; i < messages.size() ; i++)
            CHECK(messages[i]["seq"].asUInt64() == i + 1);

        // Every tab but the last was taken over, and nothing followed its close.
        if(label != 149)
        {
            REQUIRE(recorder.closedAfter(label));
            CHECK(*recorder.closedAfter(label) == messages.size());
            CHECK(messages.back()["code"] == "openElsewhere");
        }
    }
    CHECK_FALSE(recorder.closedAfter(149));
}
