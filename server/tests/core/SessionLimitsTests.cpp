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

#include <functional>
#include <string>
#include <thread>
#include <vector>

using namespace whist;
using namespace std::chrono_literals;
using std::chrono::milliseconds;

namespace
{

const std::string home = "203.0.113.5";
const std::string elsewhere = "198.51.100.7";

using test::createSolo;
using test::createTable;
using test::errors;
using test::joinTable;
using test::lastError;
using test::tableCode;
using test::waitUntil;

} // namespace

TEST_CASE("A connection over its message rate is told once, and closed when it does it again", "[limits][session]")
{
    test::Recorder recorder;
    const auto clock = std::make_shared<test::ManualClock>();
    const auto service = test::clockedService(clock, [](ServiceOptions& options)
    {
        options.limits.messagesPerSecond = 1;
        options.limits.messageBurst = 4;
    });
    test::Client client(service, recorder, 1, home);

    // The burst: hello and three more.
    client.hello();
    for(int i = 0 ; i < 3 ; i++)
        client.send(test::typed("pause"));
    CHECK(errors(recorder, 1).size() == 3);
    CHECK(lastError(recorder, 1)["code"] == "illegal");

    // The next is over, and is not carried out.
    client.send(createSolo(test::tableSettings(3, {0})));
    CHECK(service->registry().tableCount() == 0);
    REQUIRE(errors(recorder, 1).size() == 4);
    CHECK(lastError(recorder, 1)["code"] == "rateLimited");
    CHECK_FALSE(lastError(recorder, 1).isMember("detail"));
    CHECK(test::shapeError(lastError(recorder, 1)).empty());
    CHECK_FALSE(recorder.closedAfter(1));

    // Three seconds give three messages back; the one after them is over again, within 10 s.
    clock->advance(3s);
    for(int i = 0 ; i < 3 ; i++)
        client.send(test::typed("pause"));
    CHECK(errors(recorder, 1).size() == 7);
    CHECK(lastError(recorder, 1)["code"] == "illegal");
    CHECK_FALSE(recorder.closedAfter(1));

    client.send(test::typed("pause"));
    CHECK(lastError(recorder, 1)["code"] == "rateLimited");
    CHECK(recorder.closedAfter(1));

    // Nothing sent after that is answered.
    const std::size_t told = recorder.messagesTo(1).size();
    clock->advance(1h);
    client.send(test::typed("pause"));
    CHECK(recorder.messagesTo(1).size() == told);
}

TEST_CASE("An excess long after the last one is only told again", "[limits][session]")
{
    test::Recorder recorder;
    const auto clock = std::make_shared<test::ManualClock>();
    const auto service = test::clockedService(clock, [](ServiceOptions& options)
    {
        options.limits.messagesPerSecond = 1;
        options.limits.messageBurst = 2;
    });
    test::Client client(service, recorder, 1, home);

    client.hello();
    client.send(test::typed("pause"));
    client.send(test::typed("pause"));
    REQUIRE(lastError(recorder, 1)["code"] == "rateLimited");
    const std::size_t first = errors(recorder, 1).size();

    clock->advance(10s);
    client.send(test::typed("pause"));
    client.send(test::typed("pause"));
    REQUIRE(errors(recorder, 1).size() == first + 2);
    CHECK(lastError(recorder, 1)["code"] == "illegal");

    client.send(test::typed("pause"));
    CHECK(lastError(recorder, 1)["code"] == "rateLimited");
    CHECK_FALSE(recorder.closedAfter(1));

    // Told twice, 10 s apart: not yet a repeat. A third within the window is.
    client.send(test::typed("pause"));
    CHECK(recorder.closedAfter(1));
}

TEST_CASE("A frame that is not text counts against the rate, not as a bad message", "[limits][session]")
{
    test::Recorder recorder;
    const auto clock = std::make_shared<test::ManualClock>();
    const auto service = test::clockedService(clock, [](ServiceOptions& options)
    {
        options.limits.messagesPerSecond = 1;
        options.limits.messageBurst = 2;
    });
    test::Client client(service, recorder, 1, home);

    client.hello();
    client.sendBinary();
    REQUIRE(errors(recorder, 1).size() == 1);
    CHECK(lastError(recorder, 1)["code"] == "badMessage");

    client.sendBinary();
    REQUIRE(errors(recorder, 1).size() == 2);
    CHECK(lastError(recorder, 1)["code"] == "rateLimited");
}

TEST_CASE("With no limits set, nothing is held back", "[limits][session]")
{
    test::Recorder recorder;
    const auto clock = std::make_shared<test::ManualClock>();
    const auto service = test::clockedService(clock, [](ServiceOptions&) {});
    test::Client client(service, recorder, 1);

    client.hello();
    for(int i = 0 ; i < 200 ; i++)
        client.send(test::typed("pause"));
    CHECK(errors(recorder, 1).size() == 200);
    CHECK_FALSE(recorder.closedAfter(1));
}

TEST_CASE("Connections from one address are limited, and a closed one makes room", "[limits][session]")
{
    test::Recorder recorder;
    const auto clock = std::make_shared<test::ManualClock>();
    const auto service = test::clockedService(clock, [](ServiceOptions& options) { options.limits.connectionsPerAddress = 2; });

    test::Client first(service, recorder, 1, home);
    test::Client second(service, recorder, 2, home);
    first.hello();
    second.hello();
    CHECK(errors(recorder, 1).empty());
    CHECK(errors(recorder, 2).empty());

    // The third is told why and closed, before it has said anything.
    test::Client third(service, recorder, 3, home);
    REQUIRE(errors(recorder, 3).size() == 1);
    CHECK(lastError(recorder, 3)["code"] == "rateLimited");
    CHECK(lastError(recorder, 3)["detail"] == "connections");
    CHECK(test::shapeError(lastError(recorder, 3)).empty());
    CHECK(recorder.closedAfter(3) == 1u);

    // It is not answered if it says hello anyway.
    third.sendHello(std::nullopt);
    CHECK(recorder.messagesTo(3).size() == 1);
    CHECK_FALSE(third.member());

    // Another address has its own.
    test::Client other(service, recorder, 4, elsewhere);
    other.hello();
    CHECK(errors(recorder, 4).empty());

    // The refused one never held a place, so its close gives none back.
    third.drop();
    test::Client fourth(service, recorder, 5, home);
    CHECK(lastError(recorder, 5)["detail"] == "connections");

    first.drop();
    test::Client fifth(service, recorder, 6, home);
    fifth.hello();
    CHECK(errors(recorder, 6).empty());
    test::Client sixth(service, recorder, 7, home);
    CHECK(lastError(recorder, 7)["detail"] == "connections");
}

TEST_CASE("A connection that is gone without being told so still gives its place back", "[limits][session]")
{
    test::Recorder recorder;
    const auto clock = std::make_shared<test::ManualClock>();
    const auto service = test::clockedService(clock, [](ServiceOptions& options) { options.limits.connectionsPerAddress = 1; });

    {
        test::Client gone(service, recorder, 1, home);
        gone.hello();
    }
    test::Client next(service, recorder, 2, home);
    next.hello();
    CHECK(errors(recorder, 2).empty());
}

TEST_CASE("Tables made from one address are limited over an hour, solo and online together", "[limits][session]")
{
    test::Recorder recorder;
    const auto clock = std::make_shared<test::ManualClock>();
    const auto service = test::clockedService(clock, [](ServiceOptions& options) { options.limits.tablesPerAddressPerHour = 2; });

    test::Client ana(service, recorder, 1, home);
    test::Client bea(service, recorder, 2, home);
    test::Client cora(service, recorder, 3, elsewhere);
    ana.hello();
    bea.hello();
    cora.hello();

    // Two members behind one address share what it may make.
    ana.send(createSolo(test::tableSettings(3, {0})));
    bea.send(createTable("Bea"));
    CHECK(service->registry().tableCount() == 2);
    ana.send(test::typed("leaveTable"));
    CHECK(service->registry().tableCount() == 1);

    ana.send(createSolo(test::tableSettings(3, {0})));
    REQUIRE(lastError(recorder, 1)["code"] == "rateLimited");
    CHECK(lastError(recorder, 1)["detail"] == "tables");
    CHECK(test::shapeError(lastError(recorder, 1)).empty());
    CHECK(service->registry().tableCount() == 1);

    // Nobody else is held to it.
    cora.send(createSolo(test::tableSettings(3, {0})));
    CHECK(service->registry().tableCount() == 2);

    // An hour after the first two, there is room for two more.
    clock->advance(59min);
    ana.send(createTable("Ana"));
    CHECK(lastError(recorder, 1)["detail"] == "tables");
    clock->advance(1min);
    ana.send(createTable("Ana"));
    CHECK(service->registry().tableCount() == 3);
    ana.send(test::typed("leaveTable"));
    ana.send(createTable("Ana"));
    CHECK(service->registry().tableCount() == 3);
    ana.send(test::typed("leaveTable"));
    ana.send(createTable("Ana"));
    CHECK(lastError(recorder, 1)["detail"] == "tables");
}

TEST_CASE("A table that was refused does not use up what an address may make", "[limits][session]")
{
    test::Recorder recorder;
    const auto clock = std::make_shared<test::ManualClock>();
    const auto service = test::clockedService(clock, [](ServiceOptions& options) { options.limits.tablesPerAddressPerHour = 2; });
    test::Client ana(service, recorder, 1, home);
    ana.hello();

    ana.send(createSolo(test::tableSettings(3, {0})));
    CHECK(service->registry().tableCount() == 1);

    // Each of these is refused by the rules, or its settings, and none counts.
    for(int i = 0 ; i < 5 ; i++)
        ana.send(createSolo(test::tableSettings(3, {0})));
    CHECK(lastError(recorder, 1)["code"] == "illegal");
    ana.send(test::typed("leaveTable"));
    ana.send(createSolo(test::tableSettings(3, {0, 1})));
    CHECK(lastError(recorder, 1)["code"] == "badMessage");
    CHECK(service->registry().tableCount() == 0);

    ana.send(createSolo(test::tableSettings(3, {0})));
    CHECK(service->registry().tableCount() == 1);
    ana.send(test::typed("leaveTable"));
    ana.send(createSolo(test::tableSettings(3, {0})));
    CHECK(lastError(recorder, 1)["detail"] == "tables");
}

TEST_CASE("Joins that find no table are limited, and joins that do are not counted", "[limits][session]")
{
    test::Recorder recorder;
    const auto clock = std::make_shared<test::ManualClock>();
    const auto service = test::clockedService(clock, [](ServiceOptions& options)
    {
        options.limits.failedJoinsPerAddress = 3;
        options.limits.failedJoinWindow = std::chrono::minutes(10);
    });

    test::Client host(service, recorder, 1, elsewhere);
    host.hello();
    host.send(createTable("Ana", test::onlineSettings(4, 3)));
    const std::string code = tableCode(recorder, 1);

    test::Client guesser(service, recorder, 2, home);
    guesser.hello();
    for(int i = 0 ; i < 3 ; i++)
    {
        guesser.send(joinTable("ZZZZZZ", "Bea"));
        CHECK(lastError(recorder, 2)["code"] == "notFound");
    }
    REQUIRE(errors(recorder, 2).size() == 3);

    // Blocked, even for a code that exists.
    guesser.send(joinTable(code, "Bea"));
    REQUIRE(errors(recorder, 2).size() == 4);
    CHECK(lastError(recorder, 2)["code"] == "rateLimited");
    CHECK(lastError(recorder, 2)["detail"] == "joins");
    CHECK(test::shapeError(lastError(recorder, 2)).empty());
    CHECK_FALSE(service->registry().tableOf(*guesser.member()));

    // Another address is not.
    test::Client other(service, recorder, 3, "192.0.2.44");
    other.hello();
    other.send(joinTable(code, "Cora"));
    CHECK(errors(recorder, 3).empty());
    CHECK(service->registry().tableOf(*other.member()));

    // The failures age out of the window, one by one.
    clock->advance(10min);
    guesser.send(joinTable(code, "Bea"));
    CHECK(errors(recorder, 2).size() == 4);
    CHECK(service->registry().tableOf(*guesser.member()));
}

TEST_CASE("A join that finds its table is no failure", "[limits][session]")
{
    test::Recorder recorder;
    const auto clock = std::make_shared<test::ManualClock>();
    const auto service = test::clockedService(clock, [](ServiceOptions& options) { options.limits.failedJoinsPerAddress = 3; });

    test::Client host(service, recorder, 1, elsewhere);
    host.hello();
    host.send(createTable("Ana", test::onlineSettings(4, 3)));
    const std::string code = tableCode(recorder, 1);

    test::Client guest(service, recorder, 2, home);
    guest.hello();
    guest.send(joinTable("ZZZZZZ", "Bea"));
    guest.send(joinTable("YYYYYY", "Bea"));
    guest.send(joinTable(code, "Bea"));
    REQUIRE(service->registry().tableOf(*guest.member()));
    guest.send(test::typed("leaveTable"));

    // Had the join counted, this would be the third failure and the fourth join would be blocked.
    guest.send(joinTable("XXXXXX", "Bea"));
    CHECK(lastError(recorder, 2)["code"] == "notFound");
    guest.send(joinTable("WWWWWW", "Bea"));
    CHECK(lastError(recorder, 2)["detail"] == "joins");

    // Neither does a join the rules refuse for another reason.
    test::Client rest(service, recorder, 3, "192.0.2.44");
    rest.hello();
    rest.send(joinTable(code, "Cora"));
    REQUIRE(service->registry().tableOf(*rest.member()));
    for(int i = 0 ; i < 6 ; i++)
        rest.send(joinTable(code, "Cora"));
    CHECK(lastError(recorder, 3)["code"] == "illegal");
}

TEST_CASE("No more games run than may, and the one that is refused leaves nothing behind", "[limits][busy][session]")
{
    test::Recorder recorder;
    const auto clock = std::make_shared<test::ManualClock>();
    const auto service = test::clockedService(clock, [](ServiceOptions& options) { options.registry.maxRunningGames = 1; });
    Registry& registry = service->registry();

    test::Client ana(service, recorder, 1);
    test::Client bea(service, recorder, 2);
    ana.hello();
    bea.hello();

    ana.send(createSolo(test::tableSettings(3, {0})));
    CHECK(registry.runningGames() == 1);

    bea.send(createSolo(test::tableSettings(3, {0})));
    REQUIRE(lastError(recorder, 2)["code"] == "busy");
    CHECK(test::shapeError(lastError(recorder, 2)).empty());
    CHECK(registry.tableCount() == 1);
    CHECK_FALSE(registry.tableOf(*bea.member()));
    CHECK(registry.runningGames() == 1);

    // Ana leaves, her game's thread ends, and the place is free for Bea, whose refusal left her free to ask.
    ana.send(test::typed("leaveTable"));
    REQUIRE(waitUntil([&] { return registry.runningGames() == 0; }));
    bea.send(createSolo(test::tableSettings(3, {0})));
    CHECK(registry.tableOf(*bea.member()));
    CHECK(errors(recorder, 2).size() == 1);
}

TEST_CASE("A game that has finished has given its place back, though its table is still there", "[limits][busy][session]")
{
    test::Recorder recorder;
    const auto clock = std::make_shared<test::ManualClock>();
    const auto service = test::clockedService(clock, [](ServiceOptions& options) { options.registry.maxRunningGames = 1; });
    Registry& registry = service->registry();

    test::Client watcher(service, recorder, 1);
    test::Client ana(service, recorder, 2);
    watcher.hello();
    ana.hello();

    watcher.send(createSolo(test::tableSettings(3, {})));
    const std::shared_ptr<Table> table = registry.tableOf(*watcher.member());
    REQUIRE(table);
    REQUIRE(waitUntil([&] { return table->state() == TableState::Finished; }));
    REQUIRE(waitUntil([&] { return registry.runningGames() == 0; }));

    ana.send(createSolo(test::tableSettings(3, {0})));
    CHECK(registry.tableOf(*ana.member()));
    CHECK(registry.tableCount() == 2);
    CHECK(errors(recorder, 2).empty());
}

TEST_CASE("A game the host starts is refused when too many are running, and the table stays as it was", "[limits][busy][session]")
{
    test::Recorder recorder;
    const auto clock = std::make_shared<test::ManualClock>();
    const auto service = test::clockedService(clock, [](ServiceOptions& options) { options.registry.maxRunningGames = 1; });
    Registry& registry = service->registry();

    test::Client host(service, recorder, 1);
    test::Client ana(service, recorder, 2);
    host.hello();
    ana.hello();

    host.send(createTable("Host", test::onlineSettings(2, 1)));
    const std::shared_ptr<Table> table = registry.tableOf(*host.member());
    REQUIRE(table);
    ana.send(createSolo(test::tableSettings(3, {0})));
    REQUIRE(registry.runningGames() == 1);

    const std::size_t before = recorder.messagesTo(1).size();
    host.send(test::typed("startGame"));
    REQUIRE(lastError(recorder, 1)["code"] == "busy");
    CHECK(table->state() == TableState::Lobby);
    CHECK(table->game() == 0);

    // The refusal tells the host and no one else, and changes nothing the table says.
    const std::vector<Json::Value> told = recorder.messagesTo(1);
    REQUIRE(told.size() == before + 1);
    CHECK(registry.runningGames() == 1);

    ana.send(test::typed("leaveTable"));
    REQUIRE(waitUntil([&] { return registry.runningGames() == 0; }));
    host.send(test::typed("startGame"));
    CHECK(errors(recorder, 1).size() == 1);
    CHECK(table->state() == TableState::Playing);
    CHECK(registry.runningGames() == 1);
}

namespace
{

// A solo game at `speed`, waiting for its one human.
Settings soloAt(int speed)
{
    Settings settings = test::tableSettings(3, {0}, Level::Cyborg);
    settings.botSpeed = speed;
    return settings;
}

std::shared_ptr<Service> instantCappedService(const std::shared_ptr<test::ManualClock>& clock)
{
    return test::clockedService(clock, [](ServiceOptions& options)
    {
        options.registry.maxRunningGames = 3;
        options.registry.maxInstantGames = 1;
    });
}

} // namespace

TEST_CASE("No more Instant games run than may, and games at other speeds still start", "[limits][busy][instant][session]")
{
    test::Recorder recorder;
    const auto clock = std::make_shared<test::ManualClock>();
    const auto service = instantCappedService(clock);
    Registry& registry = service->registry();

    test::Client ana(service, recorder, 1);
    test::Client bea(service, recorder, 2);
    ana.hello();
    bea.hello();

    ana.send(createSolo(soloAt(fastestSpeed)));
    REQUIRE(registry.tableOf(*ana.member()));
    CHECK(registry.instantGames() == 1);

    bea.send(createSolo(soloAt(fastestSpeed)));
    REQUIRE(lastError(recorder, 2)["code"] == "busy");
    CHECK_FALSE(registry.tableOf(*bea.member()));
    CHECK(registry.runningGames() == 1);

    bea.send(createSolo(soloAt(fastestSpeed - 1)));
    CHECK(registry.tableOf(*bea.member()));
    CHECK(errors(recorder, 2).size() == 1);
    CHECK(registry.countedGames() == 2);
    CHECK(registry.instantGames() == 1);
}

TEST_CASE("A solo game is refused Instant while as many run as may, and keeps its speed", "[limits][busy][instant][session]")
{
    test::Recorder recorder;
    const auto clock = std::make_shared<test::ManualClock>();
    const auto service = instantCappedService(clock);
    Registry& registry = service->registry();

    test::Client ana(service, recorder, 1);
    test::Client bea(service, recorder, 2);
    ana.hello();
    bea.hello();
    ana.send(createSolo(soloAt(fastestSpeed)));
    bea.send(createSolo(soloAt(slowestSpeed)));
    const std::shared_ptr<Table> table = registry.tableOf(*bea.member());
    REQUIRE(table);

    Json::Value faster = test::typed("updateSettings");
    faster["settings"] = encodeSettings(soloAt(fastestSpeed));
    bea.send(faster);
    REQUIRE(lastError(recorder, 2)["code"] == "busy");
    CHECK(table->settings().botSpeed == slowestSpeed);
    CHECK(registry.instantGames() == 1);

    // Once Ana's game is gone there is room, and slowing down again gives the place back.
    ana.send(test::typed("leaveTable"));
    REQUIRE(waitUntil([&] { return registry.instantGames() == 0; }));
    bea.send(faster);
    CHECK(errors(recorder, 2).size() == 1);
    CHECK(table->settings().botSpeed == fastestSpeed);
    CHECK(registry.instantGames() == 1);

    Json::Value slower = test::typed("updateSettings");
    slower["settings"] = encodeSettings(soloAt(slowestSpeed));
    bea.send(slower);
    CHECK(registry.instantGames() == 0);
    CHECK(registry.countedGames() == 1);
}

namespace
{

// A service that allows `most` attended games, and waits ten seconds for a player who has gone.
std::shared_ptr<Service> cappedService(const std::shared_ptr<test::ManualClock>& clock, std::size_t most)
{
    return test::clockedService(clock, [most](ServiceOptions& options)
    {
        options.registry.maxRunningGames = most;
        options.registry.table.gracePeriod = 10s;
    });
}

// The tables are reaped at a time past the grace period and short of the idle limits.
void pastGrace(Service& service)
{
    service.registry().reap(Clock::now() + 11s);
}

} // namespace

TEST_CASE("A game nobody is at stops counting against the most that may run, once its grace period is over", "[limits][busy][attended][session]")
{
    constexpr std::size_t most = 3;
    test::Recorder recorder;
    const auto clock = std::make_shared<test::ManualClock>();
    const auto service = cappedService(clock, most);
    Registry& registry = service->registry();

    std::vector<std::unique_ptr<test::Client>> players;
    for(MemberId label = 1 ; label <= most ; label++)
    {
        players.push_back(std::make_unique<test::Client>(service, recorder, label));
        players.back()->hello();
        players.back()->send(createSolo(test::tableSettings(3, {0}, Level::Cyborg)));
    }
    REQUIRE(registry.countedGames() == most);

    test::Client visitor(service, recorder, 100);
    visitor.hello();
    visitor.send(createSolo(test::tableSettings(3, {0}, Level::Cyborg)));
    CHECK(lastError(recorder, 100)["code"] == "busy");

    // They go. Within the grace period their games still count.
    for(const auto& player : players)
        player->drop();
    players.clear();
    registry.reap(Clock::now() + 5s);
    CHECK(registry.countedGames() == most);
    visitor.send(createSolo(test::tableSettings(3, {0}, Level::Cyborg)));
    CHECK(errors(recorder, 100).size() == 2);

    // Past it they do not, and the games still run: they hold, and take no processor.
    pastGrace(*service);
    CHECK(registry.countedGames() == 0);
    CHECK(registry.runningGames() == most);
    ServerStatus status = service->status();
    CHECK(status.gameThreads == most);
    CHECK(status.attendedGames == 0);

    visitor.send(createSolo(test::tableSettings(3, {0}, Level::Cyborg)));
    CHECK(registry.tableOf(*visitor.member()));
    CHECK(errors(recorder, 100).size() == 2);
    CHECK(registry.countedGames() == 1);
    CHECK(registry.runningGames() == most + 1);
}

TEST_CASE("Fifty abandoned games do not keep one more from starting", "[limits][busy][attended][session]")
{
    test::Recorder recorder;
    const auto clock = std::make_shared<test::ManualClock>();
    const auto service = cappedService(clock, 2);
    Registry& registry = service->registry();

    for(MemberId label = 1 ; label <= 50 ; label++)
    {
        test::Client player(service, recorder, label);
        player.hello();
        player.send(createSolo(test::tableSettings(3, {0}, Level::Cyborg)));
        REQUIRE(registry.tableOf(*player.member()));
        player.drop();
        pastGrace(*service);
    }

    CHECK(registry.runningGames() == 50);
    CHECK(registry.countedGames() == 0);
    for(MemberId label = 1 ; label <= 50 ; label++)
        CHECK(errors(recorder, label).empty());

    // The ones who are there are what fills it.
    std::vector<std::unique_ptr<test::Client>> present;
    for(MemberId label = 101 ; label <= 103 ; label++)
    {
        present.push_back(std::make_unique<test::Client>(service, recorder, label));
        present.back()->hello();
        present.back()->send(createSolo(test::tableSettings(3, {0}, Level::Cyborg)));
    }
    CHECK(registry.tableOf(*present[0]->member()));
    CHECK(registry.tableOf(*present[1]->member()));
    CHECK_FALSE(registry.tableOf(*present[2]->member()));
    CHECK(lastError(recorder, 103)["code"] == "busy");
}

TEST_CASE("A player who comes back to their game is never refused, so the most is soft for returns, and still holds for new games", "[limits][busy][attended][session]")
{
    constexpr std::size_t most = 2;
    test::Recorder recorder;
    const auto clock = std::make_shared<test::ManualClock>();
    const auto service = cappedService(clock, most);
    Registry& registry = service->registry();

    auto ana = std::make_unique<test::Client>(service, recorder, 1);
    const std::string token = ana->hello()["token"].asString();
    ana->send(createSolo(test::tableSettings(3, {0}, Level::Cyborg)));
    test::Client bea(service, recorder, 2);
    bea.hello();
    bea.send(createSolo(test::tableSettings(3, {0}, Level::Cyborg)));
    REQUIRE(registry.countedGames() == most);

    // Ana's game stops counting, and the place is taken by Cora's.
    ana->drop();
    ana.reset();
    pastGrace(*service);
    REQUIRE(registry.countedGames() == 1);
    test::Client cora(service, recorder, 3);
    cora.hello();
    cora.send(createSolo(test::tableSettings(3, {0}, Level::Cyborg)));
    REQUIRE(registry.tableOf(*cora.member()));
    REQUIRE(registry.countedGames() == most);

    // Ana comes back and has her game, over the most. Nobody else gets one until someone leaves.
    test::Client back(service, recorder, 4);
    back.hello(token);
    CHECK(registry.tableOf(*back.member()));
    CHECK(errors(recorder, 4).empty());
    CHECK(registry.countedGames() == most + 1);
    CHECK(registry.runningGames() == most + 1);

    test::Client dan(service, recorder, 5);
    dan.hello();
    dan.send(createSolo(test::tableSettings(3, {0}, Level::Cyborg)));
    CHECK(lastError(recorder, 5)["code"] == "busy");

    // Her game goes on counting for as long as she is there, and the places come back as games end.
    back.send(test::typed("leaveTable"));
    cora.send(test::typed("leaveTable"));
    REQUIRE(waitUntil([&] { return registry.countedGames() == 1; }));
    dan.send(createSolo(test::tableSettings(3, {0}, Level::Cyborg)));
    CHECK(registry.tableOf(*dan.member()));
}

TEST_CASE("A game that ends gives back its place whether or not anyone was at it", "[limits][busy][attended][session]")
{
    test::Recorder recorder;
    const auto clock = std::make_shared<test::ManualClock>();
    const auto service = cappedService(clock, 1);
    Registry& registry = service->registry();

    test::Client watcher(service, recorder, 1);
    watcher.hello();
    watcher.send(createSolo(test::tableSettings(3, {})));
    const std::shared_ptr<Table> table = registry.tableOf(*watcher.member());
    REQUIRE(table);
    REQUIRE(waitUntil([&] { return table->state() == TableState::Finished; }));
    REQUIRE(waitUntil([&] { return registry.runningGames() == 0; }));
    CHECK(registry.countedGames() == 0);

    // A reap after the game has ended does not count it again.
    pastGrace(*service);
    CHECK(registry.countedGames() == 0);
}

namespace
{

// A service that allows `most` tables at once, and each address `quota` new ones an hour.
std::shared_ptr<Service> tablesService(const std::shared_ptr<test::ManualClock>& clock, std::size_t most, unsigned int quota = 0)
{
    return test::clockedService(clock, [most, quota](ServiceOptions& options)
    {
        options.registry.maxTables = most;
        options.limits.tablesPerAddressPerHour = quota;
    });
}

} // namespace

TEST_CASE("No more tables exist than may, whoever is at them, and a refusal for it is busy", "[limits][busy][tables][session]")
{
    constexpr std::size_t most = 3;
    test::Recorder recorder;
    const auto clock = std::make_shared<test::ManualClock>();
    const auto service = tablesService(clock, most);
    Registry& registry = service->registry();

    std::vector<std::unique_ptr<test::Client>> makers;
    for(MemberId label = 1 ; label <= most ; label++)
    {
        makers.push_back(std::make_unique<test::Client>(service, recorder, label));
        makers.back()->hello();
    }
    makers[0]->send(createTable("Ana", test::onlineSettings(2, 1)));
    makers[1]->send(createTable("Bea", test::onlineSettings(2, 1)));
    // A solo game of computer players, which nobody is at once it has begun, counts as a table too.
    makers[2]->send(createSolo(test::tableSettings(3, {})));
    REQUIRE(registry.tableCount() == most);

    test::Client visitor(service, recorder, 100);
    visitor.hello();
    visitor.send(createSolo(test::tableSettings(3, {0})));
    REQUIRE(lastError(recorder, 100)["code"] == "busy");
    CHECK(test::shapeError(lastError(recorder, 100)).empty());
    visitor.send(createTable("Dan", test::onlineSettings(2, 1)));
    CHECK(lastError(recorder, 100)["code"] == "busy");
    CHECK(errors(recorder, 100).size() == 2);

    // Nothing was left behind, and the visitor is free to ask again.
    CHECK(registry.tableCount() == most);
    CHECK_FALSE(registry.tableOf(*visitor.member()));
}

TEST_CASE("A table that closes makes room for another", "[limits][busy][tables][session]")
{
    test::Recorder recorder;
    const auto clock = std::make_shared<test::ManualClock>();
    const auto service = tablesService(clock, 1);
    Registry& registry = service->registry();

    test::Client ana(service, recorder, 1);
    test::Client bea(service, recorder, 2);
    ana.hello();
    bea.hello();
    ana.send(createTable("Ana", test::onlineSettings(2, 1)));
    REQUIRE(registry.tableOf(*ana.member()));

    bea.send(createTable("Bea", test::onlineSettings(2, 1)));
    REQUIRE(lastError(recorder, 2)["code"] == "busy");

    ana.send(test::typed("leaveTable"));
    REQUIRE(registry.tableCount() == 0);
    bea.send(createTable("Bea", test::onlineSettings(2, 1)));
    CHECK(registry.tableOf(*bea.member()));
    CHECK(errors(recorder, 2).size() == 1);
}

TEST_CASE("At the most tables, joining a table and coming back to one still work", "[limits][busy][tables][session]")
{
    test::Recorder recorder;
    const auto clock = std::make_shared<test::ManualClock>();
    const auto service = tablesService(clock, 2);
    Registry& registry = service->registry();

    auto ana = std::make_unique<test::Client>(service, recorder, 1);
    const std::string token = ana->hello()["token"].asString();
    ana->send(createTable("Ana", test::onlineSettings(3, 2)));
    const std::string code = tableCode(recorder, 1);
    test::Client cora(service, recorder, 3);
    cora.hello();
    cora.send(createSolo(test::tableSettings(3, {0})));
    REQUIRE(registry.tableCount() == 2);

    // Someone with no table joins the one that is there: that makes no table.
    test::Client bea(service, recorder, 2);
    bea.hello();
    bea.send(joinTable(code, "Bea"));
    CHECK(registry.tableOf(*bea.member()));
    CHECK(errors(recorder, 2).empty());

    // Ana's connection goes, and she comes back with her token to the table that is hers.
    ana->drop();
    ana.reset();
    test::Client back(service, recorder, 4);
    back.hello(token);
    CHECK(registry.tableOf(*back.member()));
    CHECK(errors(recorder, 4).empty());
    CHECK(registry.tableCount() == 2);
}

TEST_CASE("A busy refusal for the tables gives the address its new table back", "[limits][busy][tables][session]")
{
    test::Recorder recorder;
    const auto clock = std::make_shared<test::ManualClock>();
    const auto service = tablesService(clock, 1, 2);
    Registry& registry = service->registry();

    test::Client ana(service, recorder, 1, home);
    test::Client bea(service, recorder, 2, home);
    ana.hello();
    bea.hello();

    // Two new tables an hour: Ana's is the first, and Bea's is refused and does not use the second.
    ana.send(createTable("Ana", test::onlineSettings(2, 1)));
    bea.send(createTable("Bea", test::onlineSettings(2, 1)));
    REQUIRE(lastError(recorder, 2)["code"] == "busy");

    ana.send(test::typed("leaveTable"));
    bea.send(createTable("Bea", test::onlineSettings(2, 1)));
    CHECK(registry.tableOf(*bea.member()));
    CHECK(errors(recorder, 2).size() == 1);

    // That was the second, so a third is over the quota and not busy.
    bea.send(test::typed("leaveTable"));
    ana.send(createTable("Ana", test::onlineSettings(2, 1)));
    CHECK(lastError(recorder, 1)["code"] == "rateLimited");
    CHECK(lastError(recorder, 1)["detail"] == "tables");
}

TEST_CASE("A drain at a server with the most tables says draining, not busy", "[limits][busy][tables][drain][session]")
{
    test::Recorder recorder;
    const auto clock = std::make_shared<test::ManualClock>();
    const auto service = tablesService(clock, 1);

    test::Client ana(service, recorder, 1);
    test::Client bea(service, recorder, 2);
    ana.hello();
    bea.hello();
    ana.send(createTable("Ana", test::onlineSettings(2, 1)));

    service->beginDrain(Clock::now() + 15min);
    bea.send(createTable("Bea", test::onlineSettings(2, 1)));
    CHECK(lastError(recorder, 2)["code"] == "draining");
}
