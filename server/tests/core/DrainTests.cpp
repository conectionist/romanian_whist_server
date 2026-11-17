#include "core/Codec.h"
#include "core/Service.h"

#include "support/Client.h"
#include "support/Games.h"
#include "support/ManualClock.h"
#include "support/Online.h"
#include "support/Recorder.h"
#include "support/Requests.h"
#include "support/ServerMessages.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <latch>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using namespace whist;
using namespace std::chrono_literals;
using std::chrono::milliseconds;

namespace
{

using test::createSolo;
using test::createTable;
using test::errors;
using test::joinTable;
using test::lastError;
using test::messagesOfType;
using test::tableCode;
using test::waitUntil;

constexpr auto fifteenMinutes = 15min;

std::vector<std::string> types(const test::Recorder& recorder, MemberId label)
{
    std::vector<std::string> found;
    for(const Json::Value& message : recorder.messagesTo(label))
        found.push_back(message["type"].asString());
    return found;
}

} // namespace

TEST_CASE("Everyone connected is told the server restarts, with the time its games have", "[drain]")
{
    test::Recorder recorder;
    const auto clock = std::make_shared<test::ManualClock>();
    const auto service = test::clockedService(clock);

    test::Client ana(service, recorder, 1);
    test::Client bea(service, recorder, 2);
    ana.hello();
    bea.hello();

    // A connection that has not said hello is sent nothing, so its first message is still its welcome.
    test::Client cora(service, recorder, 3);

    service->beginDrain(clock->now() + fifteenMinutes);
    CHECK(service->draining());

    for(const MemberId label : {1, 2})
    {
        const std::vector<Json::Value> told = messagesOfType(recorder, label, "restarting");
        REQUIRE(told.size() == 1);
        CHECK(told[0]["deadline"] == 900000);
        CHECK(test::shapeError(told[0]).empty());
        CHECK(told[0]["seq"] == 3);
    }
    CHECK(recorder.messagesTo(3).empty());

    // Asked again, the server says nothing more, and keeps the first deadline.
    clock->advance(1min);
    service->beginDrain(clock->now() + 1h);
    CHECK(messagesOfType(recorder, 1, "restarting").size() == 1);
    CHECK(messagesOfType(recorder, 2, "restarting").size() == 1);
}

TEST_CASE("A connection that says hello during the drain is told right after its welcome, with what is left", "[drain]")
{
    test::Recorder recorder;
    const auto clock = std::make_shared<test::ManualClock>();
    const auto service = test::clockedService(clock);

    test::Client first(service, recorder, 1);
    first.hello();
    service->beginDrain(clock->now() + fifteenMinutes);

    // The deadline counts down from the stop. It is not given afresh to whoever comes later.
    clock->advance(1min);
    test::Client second(service, recorder, 2);
    CHECK(recorder.messagesTo(2).empty());
    second.hello();

    REQUIRE(types(recorder, 2).size() == 3);
    CHECK(types(recorder, 2) == std::vector<std::string>{"welcome", "restarting", "sync"});
    const Json::Value told = recorder.messagesTo(2)[1];
    CHECK(told["deadline"] == 840000);
    CHECK(told["seq"] == 2);
    CHECK(test::shapeError(told).empty());

    // Fractions of a second left are rounded up, so the last one still reads as time.
    clock->advance(14min - 500ms);
    test::Client third(service, recorder, 3);
    third.hello();
    CHECK(recorder.messagesTo(3)[1]["deadline"] == 500);

    // Once the time is up, nothing is left, and a negative time is never sent.
    clock->advance(10min);
    test::Client fourth(service, recorder, 4);
    fourth.hello();
    CHECK(recorder.messagesTo(4)[1]["type"] == "restarting");
    CHECK(recorder.messagesTo(4)[1]["deadline"] == 0);
}

TEST_CASE("A member who comes back to their own table during the drain is told too, and has their seat", "[drain]")
{
    test::Recorder recorder;
    const auto clock = std::make_shared<test::ManualClock>();
    const auto service = test::clockedService(clock);

    auto host = std::make_unique<test::Client>(service, recorder, 1);
    const std::string token = host->hello()["token"].asString();
    host->send(createTable("Ana"));
    const std::string code = tableCode(recorder, 1);

    service->beginDrain(clock->now() + fifteenMinutes);
    host->drop();
    host.reset();

    test::Client again(service, recorder, 2);
    again.hello(token);

    CHECK(types(recorder, 2) == std::vector<std::string>{"welcome", "restarting", "sync"});
    const Json::Value sync = recorder.messagesTo(2)[2];
    CHECK(sync["table"]["code"] == code);
    CHECK(sync["table"]["seats"][0]["you"] == true);
}

TEST_CASE("A drain refuses new tables and new games with draining, and nothing else", "[drain]")
{
    test::Recorder recorder;
    const auto clock = std::make_shared<test::ManualClock>();
    const auto service = test::clockedService(clock);
    Registry& registry = service->registry();

    test::Client host(service, recorder, 1);
    test::Client guest(service, recorder, 2);
    test::Client maker(service, recorder, 3);
    test::Client player(service, recorder, 4);
    test::Client newcomer(service, recorder, 5);
    for(test::Client* client : {&host, &guest, &maker, &player, &newcomer})
        client->hello();

    // A table that is full and waits for its host to start it, one with a seat open, and a game that
    // waits for its human.
    host.send(createTable("Host", test::onlineSettings(3, 1)));
    const std::shared_ptr<Table> waiting = registry.tableOf(*host.member());
    REQUIRE(waiting);
    maker.send(createTable("Maker", test::onlineSettings(3, 2)));
    const std::string openCode = tableCode(recorder, 3);
    player.send(createSolo(test::tableSettings(3, {0}, Level::Cyborg)));
    REQUIRE(registry.runningGames() == 1);
    const std::size_t tables = registry.tableCount();

    service->beginDrain(clock->now() + fifteenMinutes);

    // No table, solo or online, is made, and the member is left as they were.
    newcomer.send(createSolo(test::tableSettings(3, {0})));
    CHECK(lastError(recorder, 5)["code"] == "draining");
    CHECK(test::shapeError(lastError(recorder, 5)).empty());
    newcomer.send(createTable("Newcomer"));
    CHECK(lastError(recorder, 5)["code"] == "draining");
    CHECK(registry.tableCount() == tables);
    CHECK_FALSE(registry.tableOf(*newcomer.member()));

    // No game is started. A request the rules refuse for another reason is refused for that.
    guest.send(test::typed("startGame"));
    CHECK(lastError(recorder, 2)["code"] == "illegal");
    host.send(test::typed("startGame"));
    CHECK(lastError(recorder, 1)["code"] == "draining");
    CHECK(waiting->state() == TableState::Lobby);
    CHECK(waiting->game() == 0);
    CHECK(registry.runningGames() == 1);

    // Joining goes on, and so does the game that is running.
    newcomer.send(joinTable(openCode, "Newcomer"));
    CHECK(registry.tableOf(*newcomer.member()));
    CHECK(errors(recorder, 5).size() == 2);
    CHECK(registry.tableOf(*player.member())->state() == TableState::Playing);
}

TEST_CASE("A drain at a full server says draining, not busy", "[drain]")
{
    test::Recorder recorder;
    const auto clock = std::make_shared<test::ManualClock>();
    const auto service = test::clockedService(clock, [](ServiceOptions& options) { options.registry.maxRunningGames = 1; });
    Registry& registry = service->registry();

    test::Client player(service, recorder, 1);
    test::Client host(service, recorder, 2);
    player.hello();
    host.hello();
    host.send(createTable("Host", test::onlineSettings(2, 1)));
    player.send(createSolo(test::tableSettings(3, {0})));
    REQUIRE(registry.runningGames() == 1);

    host.send(test::typed("startGame"));
    CHECK(lastError(recorder, 2)["code"] == "busy");

    service->beginDrain(clock->now() + fifteenMinutes);
    host.send(test::typed("startGame"));
    CHECK(lastError(recorder, 2)["code"] == "draining");
}

TEST_CASE("Every connection is told once, whether it said hello before the drain began or during it", "[drain][race]")
{
    constexpr MemberId tabs = 40;

    for(int round = 0 ; round < 10 ; round++)
    {
        test::Recorder recorder;
        const auto clock = std::make_shared<test::ManualClock>();
        const auto service = test::clockedService(clock);

        std::vector<std::unique_ptr<test::Client>> clients;
        for(MemberId label = 1 ; label <= tabs ; label++)
            clients.push_back(std::make_unique<test::Client>(service, recorder, label));

        std::latch start(tabs + 1);
        std::vector<std::jthread> threads;
        for(MemberId label = 1 ; label <= tabs ; label++)
        {
            threads.emplace_back([&, label]
            {
                start.arrive_and_wait();
                clients[label - 1]->sendHello(std::nullopt);
            });
        }
        threads.emplace_back([&]
        {
            start.arrive_and_wait();
            service->beginDrain(clock->now() + fifteenMinutes);
        });
        threads.clear();

        for(MemberId label = 1 ; label <= tabs ; label++)
        {
            INFO("round " << round << ", tab " << label);
            const std::vector<std::string> sent = types(recorder, label);
            REQUIRE(sent.size() == 3);
            CHECK(sent.front() == "welcome");
            CHECK(std::ranges::count(sent, "restarting") == 1);
            CHECK(std::ranges::count(sent, "sync") == 1);
        }
    }
}

TEST_CASE("A game that is running when the drain begins goes on, and the server still quits when it is cut", "[drain]")
{
    test::Recorder recorder;
    const auto clock = std::make_shared<test::ManualClock>();
    const auto service = test::clockedService(clock);
    Registry& registry = service->registry();

    test::Client player(service, recorder, 1);
    player.hello();
    player.send(createSolo(test::tableSettings(3, {0}, Level::Cyborg)));
    REQUIRE(registry.runningGames() == 1);
    REQUIRE(recorder.waitFor(1, test::ofType("prompt"), test::patience));

    service->beginDrain(clock->now() + fifteenMinutes);
    CHECK(registry.runningGames() == 1);

    // Ending the games at the deadline is shutdown(), as it always was: every game is abandoned and joined.
    service->shutdown();
    CHECK(registry.runningGames() == 0);
}
