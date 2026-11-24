#include "core/Codec.h"
#include "core/Service.h"

#include "support/Client.h"
#include "support/Games.h"
#include "support/ManualClock.h"
#include "support/Online.h"
#include "support/Recorder.h"
#include "support/Requests.h"
#include "support/ScriptedHuman.h"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <thread>

using namespace whist;
using namespace std::chrono_literals;
using std::chrono::milliseconds;
using test::createSolo;
using test::lastError;
using test::messagesOfType;
using test::ofType;
using test::patience;

namespace
{

// The server's own defaults. The reaper is called by hand with times this far ahead, so none of the
// tests waits for them.
constexpr std::chrono::minutes waitedOn{5};
constexpr std::chrono::seconds toAnswer{30};
constexpr std::chrono::seconds grace{30};
constexpr std::chrono::minutes soloIdle{10};
constexpr std::chrono::minutes onlineIdle{5};

void askAfterWaits(ServiceOptions& options)
{
    options.registry.table.stillHereAfter = waitedOn;
    options.registry.table.stillHereAnswer = toAnswer;
    options.registry.table.gracePeriod = grace;
    options.registry.table.soloIdle = soloIdle;
    options.registry.table.onlineIdle = onlineIdle;
}

std::shared_ptr<Service> askingService(const std::shared_ptr<test::ManualClock>& clock,
                                       const std::function<void(ServiceOptions&)>& tweak = {})
{
    return test::clockedService(clock, [&](ServiceOptions& options)
    {
        askAfterWaits(options);
        if(tweak)
            tweak(options);
    });
}

Settings soloAt(int speed)
{
    Settings settings = test::tableSettings(3, {0}, Level::Cyborg);
    settings.botSpeed = speed;
    return settings;
}

std::string presence(const Table& table, MemberId member, unsigned int seat)
{
    return encodeTableView(table.viewFor(member))["seats"][seat]["presence"].asString();
}

// The presence of a seat in the last tableState the label was sent.
std::string toldPresence(const test::Recorder& recorder, MemberId label, unsigned int seat)
{
    const std::vector<Json::Value> states = messagesOfType(recorder, label, "tableState");
    return states.empty() ? std::string() : states.back()["seats"][seat]["presence"].asString();
}

Json::Value goodAnswer(const Json::Value& prompt)
{
    const bool bet = prompt["kind"] == "bet";
    Json::Value answer = test::typed(bet ? "bet" : "play");
    answer["prompt"] = prompt["id"];
    if(bet)
        answer["value"] = prompt["forbidden"] == 0 ? 1 : 0;
    else
        answer["index"] = prompt["legal"][0];
    return answer;
}

} // namespace

TEST_CASE("Solo: a turn left open is asked about, and no answer makes the player away; the game stops counting, then closes",
          "[stillhere][solo]")
{
    test::Recorder recorder;
    const auto clock = std::make_shared<test::ManualClock>();
    const auto service = askingService(clock);
    Registry& registry = service->registry();

    test::Client ana(service, recorder, 1);
    ana.hello();
    ana.send(createSolo(test::tableSettings(3, {0})));
    REQUIRE(recorder.waitFor(1, ofType("prompt"), patience));
    const std::shared_ptr<Table> table = registry.tableOf(*ana.member());
    REQUIRE(table);
    const Clock::time_point start = Clock::now();

    // Thinking, even for a long time, is not asked about.
    registry.reap(start + waitedOn - 1s);
    CHECK(messagesOfType(recorder, 1, "stillHere").empty());

    const Clock::time_point asked = start + waitedOn + 1s;
    registry.reap(asked);
    const std::vector<Json::Value> questions = messagesOfType(recorder, 1, "stillHere");
    REQUIRE(questions.size() == 1);
    CHECK(questions[0]["deadline"] == 30000);
    CHECK(test::shapeError(questions[0]).empty());

    registry.reap(asked + toAnswer - 1s);
    CHECK(presence(*table, *ana.member(), 0) == "here");

    // No answer: away, as if the tab had closed. It is still open, so it is told.
    const Clock::time_point gone = asked + toAnswer;
    registry.reap(gone);
    CHECK(presence(*table, *ana.member(), 0) == "away");
    CHECK(toldPresence(recorder, 1, 0) == "away");
    CHECK(registry.countedGames() == 1);
    CHECK(messagesOfType(recorder, 1, "stillHere").size() == 1);

    // The game holds, and after the grace period it no longer counts against the most that may run.
    registry.reap(gone + grace - 1s);
    CHECK(registry.countedGames() == 1);
    registry.reap(gone + grace);
    CHECK(registry.countedGames() == 0);

    registry.reap(gone + soloIdle - 1s);
    CHECK(registry.tableCount() == 1);
    registry.reap(gone + soloIdle);
    CHECK(registry.tableCount() == 0);
    const std::vector<Json::Value> states = messagesOfType(recorder, 1, "tableState");
    REQUIRE_FALSE(states.empty());
    CHECK(states.back()["state"] == "closed");

    // At no table her open tab is simply connected, and her next game does not hold for nobody.
    ana.send(createSolo(test::tableSettings(3, {0})));
    const std::shared_ptr<Table> next = registry.tableOf(*ana.member());
    REQUIRE(next);
    CHECK(presence(*next, *ana.member(), 0) == "here");
}

TEST_CASE("Solo: `back` in time keeps the player here, a pong does not, and a move brings an away player back",
          "[stillhere][solo]")
{
    test::Recorder recorder;
    const auto clock = std::make_shared<test::ManualClock>();
    const auto service = askingService(clock);
    Registry& registry = service->registry();

    test::Client ana(service, recorder, 1);
    ana.hello();
    ana.send(createSolo(test::tableSettings(3, {0})));
    const std::optional<Json::Value> prompt = recorder.waitFor(1, ofType("prompt"), patience);
    REQUIRE(prompt);
    const std::shared_ptr<Table> table = registry.tableOf(*ana.member());
    REQUIRE(table);
    const Clock::time_point start = Clock::now();

    const Clock::time_point asked = start + waitedOn + 1s;
    registry.reap(asked);
    REQUIRE(messagesOfType(recorder, 1, "stillHere").size() == 1);
    ana.send(test::typed("back"));
    CHECK(test::errors(recorder, 1).empty());

    // Answered: the time to answer passes, and she is here. The game has waited long enough since
    // she last said anything for her to be asked again.
    registry.reap(asked + toAnswer);
    CHECK(presence(*table, *ana.member(), 0) == "here");
    REQUIRE(messagesOfType(recorder, 1, "stillHere").size() == 2);

    // A pong is the browser's, not hers.
    ana.heard();
    registry.reap(asked + toAnswer * 2);
    CHECK(presence(*table, *ana.member(), 0) == "away");

    // Her move is taken, and she is back.
    const std::size_t before = recorder.messagesTo(1).size();
    ana.send(goodAnswer(*prompt));
    CHECK(presence(*table, *ana.member(), 0) == "here");
    CHECK(toldPresence(recorder, 1, 0) == "here");
    const auto closed = recorder.waitFor(1, [&](const Json::Value& m)
    {
        return m["type"] == "promptClosed" && m["id"] == (*prompt)["id"];
    }, patience);
    REQUIRE(closed);
    CHECK((*closed)["by"] == "you");
    CHECK(recorder.messagesTo(1).size() > before);
    CHECK(test::errors(recorder, 1).empty());
}

TEST_CASE("Solo: a pause, or a round held for Continue, is a wait on the player too", "[stillhere][solo]")
{
    test::Recorder recorder;
    const auto clock = std::make_shared<test::ManualClock>();
    const auto service = askingService(clock);
    Registry& registry = service->registry();

    SECTION("a pause")
    {
        test::Client ana(service, recorder, 1);
        ana.hello();
        ana.send(createSolo(test::tableSettings(3, {0})));
        const std::optional<Json::Value> prompt = recorder.waitFor(1, ofType("prompt"), patience);
        REQUIRE(prompt);
        ana.send(test::typed("pause"));
        ana.send(goodAnswer(*prompt));
        REQUIRE(recorder.waitFor(1, ofType("promptClosed"), patience));

        // The game holds at its next rest, with no turn of hers open.
        std::this_thread::sleep_for(test::quiet);
        const Clock::time_point paused = Clock::now();
        registry.reap(paused + waitedOn - 1s);
        CHECK(messagesOfType(recorder, 1, "stillHere").empty());
        registry.reap(paused + waitedOn + 1s);
        CHECK(messagesOfType(recorder, 1, "stillHere").size() == 1);
    }

    SECTION("a round held for Continue")
    {
        test::Client ana(service, recorder, 1);
        ana.hello();
        Settings settings = test::tableSettings(2, {0});
        settings.waitAfterRound = true;
        ana.send(createSolo(settings));
        test::ScriptedHuman human(recorder, 1, ana.answerer(), test::Behaviour::Good);
        REQUIRE(recorder.waitFor(1, ofType("held"), patience));

        const Clock::time_point held = Clock::now();
        registry.reap(held + waitedOn - 1s);
        CHECK(messagesOfType(recorder, 1, "stillHere").empty());
        registry.reap(held + waitedOn + 1s);
        CHECK(messagesOfType(recorder, 1, "stillHere").size() == 1);
        registry.closeTable(registry.tableOf(*ana.member())->code());
    }
}

TEST_CASE("A player who went away with the tab open comes back with anything they send, even past the most games",
          "[stillhere][limits]")
{
    test::Recorder recorder;
    const auto clock = std::make_shared<test::ManualClock>();
    const auto service = askingService(clock, [](ServiceOptions& options) { options.registry.maxRunningGames = 1; });
    Registry& registry = service->registry();

    test::Client ana(service, recorder, 1);
    test::Client bea(service, recorder, 2);
    test::Client cora(service, recorder, 3);
    ana.hello();
    bea.hello();
    cora.hello();
    ana.send(createSolo(test::tableSettings(3, {0})));
    REQUIRE(recorder.waitFor(1, ofType("prompt"), patience));
    const std::shared_ptr<Table> table = registry.tableOf(*ana.member());
    REQUIRE(table);

    const Clock::time_point gone = Clock::now() + waitedOn + 1s + toAnswer;
    registry.reap(gone - toAnswer);
    registry.reap(gone);
    registry.reap(gone + grace);
    REQUIRE(registry.countedGames() == 0);

    // Her place is free for someone else.
    bea.send(createSolo(test::tableSettings(3, {0})));
    REQUIRE(registry.tableOf(*bea.member()));
    CHECK(registry.countedGames() == 1);

    // She is never refused, though that makes one more than may run.
    ana.send(test::typed("back"));
    CHECK(presence(*table, *ana.member(), 0) == "here");
    CHECK(registry.countedGames() == 2);

    cora.send(createSolo(test::tableSettings(3, {0})));
    CHECK(lastError(recorder, 3)["code"] == "busy");
}

namespace
{

struct Asking : test::Online
{
    Asking()
        : Online(milliseconds(30000), [](ServiceOptions& options)
        {
            askAfterWaits(options);
            options.registry.table.turnTimerOverride = milliseconds(150 * test::slowdown);
        })
    {
    }

    Registry& registry() const
    {
        return service->registry();
    }

    std::shared_ptr<Table> theTable() const
    {
        return registry().findTable(code);
    }
};

template <class Predicate>
bool eventually(Predicate holds)
{
    return test::waitUntil(holds);
}

bool presenceIs(const Json::Value& message, unsigned int seat, const std::string& presence)
{
    return message["type"] == "tableState" && message["seats"][seat]["presence"] == presence;
}

} // namespace

TEST_CASE("Online with a turn timer: the turns that replace a player ask whether she is still here, and a game nobody is left at stops counting and closes",
          "[stillhere][online]")
{
    Asking o;
    o.open(1);
    // Fast enough for her turns to come round soon, and slow enough that the game is still on when
    // the table asks.
    Settings settings = test::onlineSettings(3, 1);
    settings.botSpeed = fastestSpeed - 1;
    o.create(1, settings, "Ana");

    test::ScriptedHuman ana(o.recorder, 1, o.answerer(1), test::Behaviour::Silent);
    o.send(1, test::typed("startGame"));
    REQUIRE(o.recorder.waitFor(1, [](const Json::Value& m) { return presenceIs(m, 0, "replaced"); }, patience));
    REQUIRE(eventually([&] { return ana.counts().closedByComputer == 3; }));

    // The table asks at the next reap, and not before her turns ran out.
    CHECK(messagesOfType(o.recorder, 1, "stillHere").empty());
    const Clock::time_point asked = Clock::now();
    o.registry().reap(asked);
    REQUIRE(messagesOfType(o.recorder, 1, "stillHere").size() == 1);

    // The computers play her seat; the game counts for as long as she may still be there.
    o.registry().reap(asked + toAnswer - 1s);
    CHECK(o.registry().countedGames() == 1);
    const Clock::time_point gone = asked + toAnswer;
    o.registry().reap(gone);
    o.registry().reap(gone + grace - 1s);
    CHECK(o.registry().countedGames() == 1);
    o.registry().reap(gone + grace);
    CHECK(o.registry().countedGames() == 0);

    o.registry().reap(gone + onlineIdle - 1s);
    CHECK(o.theTable());
    o.registry().reap(gone + onlineIdle);
    CHECK_FALSE(o.theTable());
    CHECK(ana.problems().empty());
}

TEST_CASE("Online with no turn timer: a turn left open asks, and with no answer the others play on after the grace period",
          "[stillhere][online]")
{
    Asking o;
    o.open(1);
    o.open(2);
    Settings settings = test::onlineSettings(3, 2);
    settings.turnTimer = milliseconds(0);
    o.create(1, settings, "Ana");
    o.ask(2, o.join("Bea"), ofType("sync"));

    test::ScriptedHuman ana(o.recorder, 1, o.answerer(1), test::Behaviour::Good);
    test::ScriptedHuman bea(o.recorder, 2, o.answerer(2), test::Behaviour::Silent);
    o.send(1, test::typed("startGame"));
    REQUIRE(bea.waitForPrompt(patience));
    const Clock::time_point start = Clock::now();
    const std::size_t prompts = ana.counts().prompts;

    const Clock::time_point asked = start + waitedOn + 1s;
    o.registry().reap(asked);
    CHECK(messagesOfType(o.recorder, 2, "stillHere").size() == 1);
    CHECK(messagesOfType(o.recorder, 1, "stillHere").empty());

    // Away: Ana is told, and the table waits for Bea's turn through the grace period.
    const std::size_t before = o.mark(1);
    const Clock::time_point gone = asked + toAnswer;
    o.registry().reap(gone);
    REQUIRE(o.waitFrom(1, before, [](const Json::Value& m) { return presenceIs(m, 1, "away"); }));

    // Then her seat is the computer's, and the game goes on.
    o.registry().reap(gone + grace);
    REQUIRE(o.waitFrom(1, before, [](const Json::Value& m) { return presenceIs(m, 1, "replaced"); }));
    REQUIRE(eventually([&] { return ana.counts().prompts > prompts; }));

    // Anything she sends takes it back.
    const std::size_t back = o.mark(1);
    o.send(2, test::typed("back"));
    REQUIRE(o.waitFrom(1, back, [](const Json::Value& m) { return presenceIs(m, 1, "here"); }));
    CHECK(ana.problems().empty());
    CHECK(bea.problems().empty());
}

TEST_CASE("A player who went away with the tab open and then leaves is here at the next table", "[stillhere][online]")
{
    Asking o;
    o.open(1);
    o.open(2);
    Settings settings = test::onlineSettings(3, 2);
    settings.turnTimer = milliseconds(0);
    o.create(1, settings, "Ana");
    o.ask(2, o.join("Bea"), ofType("sync"));

    test::ScriptedHuman ana(o.recorder, 1, o.answerer(1), test::Behaviour::Good);
    test::ScriptedHuman bea(o.recorder, 2, o.answerer(2), test::Behaviour::Silent);
    o.send(1, test::typed("startGame"));
    REQUIRE(bea.waitForPrompt(patience));
    const Clock::time_point asked = Clock::now() + waitedOn + 1s;
    o.registry().reap(asked);
    o.registry().reap(asked + toAnswer);
    REQUIRE(eventually([&] { return presence(*o.theTable(), *o.tab(1).member(), 1) == "away"; }));

    const MemberId member = *o.tab(2).member();
    o.send(2, test::typed("leaveTable"));
    o.send(2, createSolo(test::tableSettings(3, {0})));
    const std::shared_ptr<Table> solo = o.registry().tableOf(member);
    REQUIRE(solo);
    CHECK(solo->mode() == Mode::Solo);
    CHECK(presence(*solo, member, 0) == "here");
}

TEST_CASE("One address runs at most its number of Instant games; another address, or another speed, still starts",
          "[stillhere][limits][instant][address]")
{
    test::Recorder recorder;
    const auto clock = std::make_shared<test::ManualClock>();
    const auto service = test::clockedService(clock, [](ServiceOptions& options)
    {
        options.registry.maxRunningGames = 10;
        options.registry.maxInstantGames = 4;
        options.registry.maxInstantGamesPerAddress = 1;
        options.limits.tablesPerAddressPerHour = 2;
    });
    Registry& registry = service->registry();

    test::Client ana(service, recorder, 1, "10.0.0.1");
    test::Client anaAgain(service, recorder, 2, "10.0.0.1");
    test::Client bea(service, recorder, 3, "10.0.0.2");
    ana.hello();
    anaAgain.hello();
    bea.hello();

    ana.send(createSolo(soloAt(fastestSpeed)));
    REQUIRE(registry.tableOf(*ana.member()));

    // Refused as the address's own limit, and the table it would have made is not counted against it.
    anaAgain.send(createSolo(soloAt(fastestSpeed)));
    CHECK(lastError(recorder, 2)["code"] == "rateLimited");
    CHECK(lastError(recorder, 2)["detail"] == "instant");
    CHECK_FALSE(registry.tableOf(*anaAgain.member()));
    anaAgain.send(createSolo(soloAt(slowestSpeed)));
    const std::shared_ptr<Table> slower = registry.tableOf(*anaAgain.member());
    REQUIRE(slower);
    CHECK(test::errors(recorder, 2).size() == 1);

    bea.send(createSolo(soloAt(fastestSpeed)));
    CHECK(registry.tableOf(*bea.member()));
    CHECK(registry.instantGames() == 2);

    // A switch to Instant is refused the same way, and the game keeps its speed.
    Json::Value faster = test::typed("updateSettings");
    faster["settings"] = encodeSettings(soloAt(fastestSpeed));
    anaAgain.send(faster);
    CHECK(lastError(recorder, 2)["code"] == "rateLimited");
    CHECK(lastError(recorder, 2)["detail"] == "instant");
    CHECK(slower->settings().botSpeed == slowestSpeed);

    // Once the address's first game is over, its place is free.
    ana.send(test::typed("leaveTable"));
    anaAgain.send(faster);
    CHECK(test::errors(recorder, 2).size() == 2);
    CHECK(slower->settings().botSpeed == fastestSpeed);
    CHECK(registry.instantGames() == 2);
}

TEST_CASE("An online Instant game counts as its host's address's", "[stillhere][limits][instant][address]")
{
    test::Recorder recorder;
    const auto clock = std::make_shared<test::ManualClock>();
    const auto service = test::clockedService(clock, [](ServiceOptions& options)
    {
        options.registry.maxInstantGamesPerAddress = 1;
    });
    Registry& registry = service->registry();

    test::Client solo(service, recorder, 1, "10.0.0.1");
    test::Client host(service, recorder, 2, "10.0.0.1");
    test::Client guest(service, recorder, 3, "10.0.0.2");
    solo.hello();
    host.hello();
    guest.hello();
    solo.send(createSolo(soloAt(fastestSpeed)));
    REQUIRE(registry.instantGames() == 1);

    Settings people = test::onlineSettings(2, 2);
    REQUIRE(people.botSpeed == fastestSpeed);
    host.send(test::createTable("Host", people));
    const std::shared_ptr<Table> table = registry.tableOf(*host.member());
    REQUIRE(table);
    guest.send(test::joinTable(table->code(), "Guest"));
    host.send(test::typed("startGame"));
    CHECK(lastError(recorder, 2)["code"] == "rateLimited");
    CHECK(lastError(recorder, 2)["detail"] == "instant");
    CHECK(table->state() == TableState::Lobby);
}
