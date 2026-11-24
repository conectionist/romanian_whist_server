#include "core/Codec.h"
#include "core/GameRun.h"
#include "core/Registry.h"
#include "core/Service.h"

#include "support/Client.h"
#include "support/Fixtures.h"
#include "support/Games.h"
#include "support/ManualClock.h"
#include "support/Recorder.h"
#include "support/Requests.h"
#include "support/ScriptedHuman.h"
#include "support/ServerMessages.h"

#include <catch2/catch_test_macros.hpp>

#include <map>
#include <random>

using namespace whist;
using std::chrono::milliseconds;

namespace
{

const milliseconds patience(60000 * test::slowdown);

// One sample of each kind of message, named as its fixture file is. Each was sent either by a game
// thread or while its game was waiting on the human, so its seq is the same on every run.
using Samples = std::map<std::string, Json::Value>;

std::string sampleName(const Json::Value& message)
{
    const std::string type = message["type"].asString();
    if(type == "game")
        return "game." + message["event"]["kind"].asString();
    if(type == "prompt")
        return "prompt." + message["kind"].asString() + (message.isMember("hand") ? "" : "-blind");
    if(type == "promptClosed")
        return "promptClosed." + message["by"].asString();

    // The errors that carry something of their own are named for it; any other is "error".
    if(type == "error" && (message["code"] == "rateLimited" || message["code"] == "busy" || message["code"] == "draining"))
        return "error." + message["code"].asString();
    return type;
}

void keepFirsts(const std::vector<Json::Value>& messages, Samples& samples)
{
    for(const Json::Value& message : messages)
        samples.try_emplace(sampleName(message), message);
}

// A seeded solo game, played through a session as a browser plays it: one human, who waits after
// each round, reloads once and sends one illegal answer.
void soloGame(Samples& samples)
{
    test::Recorder recorder;

    ServiceOptions options;
    options.registry.table.instantFloor = milliseconds(0);
    options.registry.table.threadNiceness = 0;
    options.registry.random = [random = std::mt19937_64(2026)]() mutable { return random(); };
    options.version = "0.1.0";
    options.commit = "0123abc";
    const auto service = std::make_shared<Service>(std::move(options));

    constexpr MemberId tab = 1;
    test::Client client(service, recorder, tab);
    client.hello();
    const MemberId ana = *client.member();

    Settings settings = test::tableSettings(3, {0}, Level::Cyborg);
    settings.seed = 7;
    settings.waitAfterRound = true;

    int prompts = 0;
    const auto onPrompt = [&](const Json::Value& prompt)
    {
        prompts++;
        if(prompts == 30)
            service->registry().tableOf(ana)->sync(ana);
        if(prompts == 12)
        {
            Json::Value bet = test::typed("bet");
            bet["prompt"] = prompt["id"];
            bet["value"] = 99;
            client.send(bet);
            return true;
        }
        return false;
    };

    Json::Value create = test::typed("createSolo");
    create["settings"] = encodeSettings(settings);
    client.send(create);

    test::ScriptedHuman human(recorder, tab, client.answerer(), test::Behaviour::Good, onPrompt, client.continuer());
    REQUIRE(human.waitUntilOver(patience));
    CHECK(human.problems().empty());

    // The sync that answers hello is for a member at no table; the one taken mid-game is the sample.
    const std::vector<Json::Value> messages = recorder.messagesTo(tab);
    REQUIRE(messages.size() > 2);
    samples.try_emplace("welcome", messages[0]);
    keepFirsts(std::vector<Json::Value>(messages.begin() + 2, messages.end()), samples);
}

// Online, with a turn timer the human never beats.
void timedOutGame(Samples& samples)
{
    test::Recorder recorder;
    Outbox outbox;
    outbox.attach(1, recorder.connect(1));

    Settings settings = test::tableSettings(2, {0});
    settings.replacement = Level::ReckonerBeta;
    settings.turnTimer = milliseconds(1);
    GameRunSetup setup = test::quickGame(settings, {1});

    GameRun run(std::move(setup), outbox);
    run.start();
    REQUIRE(recorder.waitFor(1, [](const Json::Value& m) { return m["type"] == "promptClosed"; }, patience));
    run.close();
    keepFirsts(recorder.messagesTo(1), samples);
}

// A game of computer players, watched, and stopped partway through its second round.
void stoppedGame(Samples& samples)
{
    struct Stopper : romanian_whist::IGameObserver
    {
        GameRun* run = nullptr;

        void onTrickWon(const romanian_whist::GameEngine& engine, romanian_whist::Seat, unsigned int trick) override
        {
            if(engine.getCurrentRoundIndex() == 1 && trick == 1)
                run->close();
        }
    };

    test::Recorder recorder;
    Outbox outbox;
    outbox.attach(5, recorder.connect(5));

    Stopper stopper;
    GameRunSetup setup = test::quickGame(test::tableSettings(3, {}), {});
    setup.watchers = {5};
    setup.firstObservers = {&stopper};
    GameRun run(std::move(setup), outbox);
    stopper.run = &run;
    run.start();
    REQUIRE(recorder.waitFor(5, [](const Json::Value& m) { return m["event"]["kind"] == "gameStopped"; }, patience));

    for(const Json::Value& message : recorder.messagesTo(5))
    {
        if(message["event"]["kind"] == "gameStopped")
            samples.try_emplace("game.gameStopped", message);
    }
}

// An online table made, joined and started step by step, so every message before the first hold has
// the same seq on every run. The hold's time left is the one thing that varies, so it is pinned.
void onlineGame(Samples& samples)
{
    test::Recorder recorder;

    ServiceOptions options;
    options.registry.table.instantFloor = milliseconds(0);
    options.registry.table.threadNiceness = 0;
    options.registry.random = [random = std::mt19937_64(2027)]() mutable { return random(); };
    const auto service = std::make_shared<Service>(std::move(options));

    test::Client ana(service, recorder, 1);
    test::Client bogdan(service, recorder, 2);
    ana.hello();
    bogdan.hello();

    Settings settings = test::tableSettings(3, {0, 1}, Level::Cyborg);
    settings.seats[0] = SeatSetting{};
    settings.seats[1] = SeatSetting{};
    settings.waitAfterRound = true;

    Json::Value create = test::typed("createTable");
    create["settings"] = encodeSettings(settings);
    create["profile"]["name"] = "Ana";
    create["profile"]["gender"] = "female";
    create["profile"]["avatar"] = "woman-long-red-hair";
    ana.send(create);
    const std::string code = recorder.messagesTo(1).back()["table"]["code"].asString();

    Json::Value join = test::typed("joinTable");
    join["code"] = code;
    join["profile"]["name"] = "Bogdan";
    join["profile"]["gender"] = "male";
    join["profile"]["avatar"] = "man-dark-hair-beard";
    bogdan.send(join);
    const auto joined = recorder.waitFor(1, [](const Json::Value& m) { return m["type"] == "tableState"; }, patience);
    REQUIRE(joined);
    samples.try_emplace("tableState.online", *joined);

    test::ScriptedHuman first(recorder, 1, ana.answerer(), test::Behaviour::Good);
    test::ScriptedHuman second(recorder, 2, bogdan.answerer(), test::Behaviour::Good);
    ana.send(test::typed("startGame"));

    auto held = recorder.waitFor(1, [](const Json::Value& m) { return m["type"] == "held"; }, patience);
    REQUIRE(held);
    CHECK((*held)["deadline"].asInt64() <= 30000);
    (*held)["deadline"] = 30000;
    samples.try_emplace("held.online", *held);

    service->registry().closeTable(code);
    CHECK(first.waitUntilOver(patience));
    CHECK(second.waitUntilOver(patience));
}

// Online: the second human's tab closes while the game waits on them, and the grace period runs out.
// The time left on the wait is the one thing that varies, so it is pinned.
void droppedGame(Samples& samples)
{
    test::Recorder recorder;

    ServiceOptions options;
    options.registry.table.instantFloor = milliseconds(0);
    options.registry.table.threadNiceness = 0;
    options.registry.table.gracePeriod = milliseconds(300 * test::slowdown);
    options.registry.random = [random = std::mt19937_64(2028)]() mutable { return random(); };
    const auto service = std::make_shared<Service>(std::move(options));

    test::Client ana(service, recorder, 1);
    test::Client bogdan(service, recorder, 2);
    ana.hello();
    bogdan.hello();

    Settings settings = test::tableSettings(3, {0, 1}, Level::Cyborg);
    settings.seats[0] = SeatSetting{};
    settings.seats[1] = SeatSetting{};

    Json::Value create = test::typed("createTable");
    create["settings"] = encodeSettings(settings);
    create["profile"]["name"] = "Ana";
    create["profile"]["gender"] = "female";
    create["profile"]["avatar"] = "woman-long-red-hair";
    ana.send(create);
    const std::string code = recorder.messagesTo(1).back()["table"]["code"].asString();

    Json::Value join = test::typed("joinTable");
    join["code"] = code;
    join["profile"]["name"] = "Bogdan";
    join["profile"]["gender"] = "male";
    join["profile"]["avatar"] = "man-dark-hair-beard";
    bogdan.send(join);

    test::ScriptedHuman first(recorder, 1, ana.answerer(), test::Behaviour::Good);
    test::ScriptedHuman second(recorder, 2, bogdan.answerer(), test::Behaviour::Silent);
    ana.send(test::typed("startGame"));
    REQUIRE(second.waitForPrompt(patience));
    bogdan.drop();

    auto waiting = recorder.waitFor(1, [](const Json::Value& m) { return m["type"] == "tableState" && m.isMember("waitingFor"); },
                                    patience);
    REQUIRE(waiting);
    CHECK((*waiting)["waitingFor"]["deadline"].asInt64() <= 300 * test::slowdown);
    (*waiting)["waitingFor"]["deadline"] = 300;
    samples.try_emplace("tableState.waiting", *waiting);

    const auto replaced = recorder.waitFor(1, [](const Json::Value& m)
    {
        return m["type"] == "tableState" && m["seats"][1]["presence"] == "replaced";
    }, patience);
    REQUIRE(replaced);
    samples.try_emplace("tableState.replaced", *replaced);

    service->registry().closeTable(code);
    CHECK(first.waitUntilOver(patience));
}

// Members over what the server allows: a second table within the hour from one address, and a game
// started while another runs. Neither sends a game message to the connection that is refused, so
// the refusals' seq is the same on every run.
void limitedGame(Samples& samples)
{
    test::Recorder recorder;

    ServiceOptions options;
    options.registry.table.instantFloor = milliseconds(0);
    options.registry.table.threadNiceness = 0;
    options.registry.maxRunningGames = 1;
    options.limits.tablesPerAddressPerHour = 1;
    const auto service = std::make_shared<Service>(std::move(options));

    test::Client maker(service, recorder, 1, "203.0.113.5");
    maker.hello();

    Settings settings = test::tableSettings(3, {0, 1}, Level::Cyborg);
    settings.seats[0] = SeatSetting{};
    settings.seats[1] = SeatSetting{};
    Json::Value create = test::typed("createTable");
    create["settings"] = encodeSettings(settings);
    create["profile"]["name"] = "Ana";
    create["profile"]["gender"] = "female";
    create["profile"]["avatar"] = "woman-long-red-hair";
    maker.send(create);
    maker.send(test::typed("leaveTable"));
    maker.send(create);
    keepFirsts(recorder.messagesTo(1), samples);

    test::Client player(service, recorder, 2, "198.51.100.7");
    test::Client crowd(service, recorder, 3, "192.0.2.44");
    player.hello();
    crowd.hello();

    Json::Value solo = test::typed("createSolo");
    solo["settings"] = encodeSettings(test::tableSettings(3, {0}, Level::Cyborg));
    player.send(solo);
    crowd.send(solo);
    keepFirsts(recorder.messagesTo(3), samples);
}

// A server that has begun to drain: a connection is told, and a game is refused. Nothing here starts
// a game thread, so the seq of both is the same on every run.
void drainingGame(Samples& samples)
{
    test::Recorder recorder;
    const auto clock = std::make_shared<test::ManualClock>();
    const auto service = test::clockedService(clock);

    test::Client ana(service, recorder, 1);
    ana.hello();
    service->beginDrain(clock->now() + std::chrono::minutes(15));

    Json::Value solo = test::typed("createSolo");
    solo["settings"] = encodeSettings(test::tableSettings(3, {0}, Level::Cyborg));
    ana.send(solo);
    keepFirsts(recorder.messagesTo(1), samples);
}

// A player asked whether they are still here. The game waits on their first turn, so the question's
// seq is the same on every run.
void askedGame(Samples& samples)
{
    test::Recorder recorder;

    ServiceOptions options;
    options.registry.table.instantFloor = milliseconds(0);
    options.registry.table.threadNiceness = 0;
    options.registry.table.stillHereAfter = std::chrono::minutes(5);
    options.registry.table.stillHereAnswer = std::chrono::seconds(30);
    const auto service = std::make_shared<Service>(std::move(options));

    test::Client client(service, recorder, 1);
    client.hello();
    Settings settings = test::tableSettings(3, {0}, Level::Cyborg);
    settings.seed = 7;
    Json::Value create = test::typed("createSolo");
    create["settings"] = encodeSettings(settings);
    client.send(create);
    REQUIRE(recorder.waitFor(1, [](const Json::Value& m) { return m["type"] == "prompt"; }, patience));

    service->registry().reap(Clock::now() + std::chrono::minutes(6));
    keepFirsts(test::messagesOfType(recorder, 1, "stillHere"), samples);
    service->registry().closeTable(service->registry().tableOf(*client.member())->code());
}

} // namespace

TEST_CASE("Every message the server sends has a fixture, and it is up to date", "[fixtures]")
{
    Samples samples;
    soloGame(samples);
    timedOutGame(samples);
    stoppedGame(samples);
    onlineGame(samples);
    droppedGame(samples);
    limitedGame(samples);
    drainingGame(samples);
    askedGame(samples);

    const std::vector<std::string> expected{
        "welcome", "tableState", "tableState.online", "tableState.waiting", "tableState.replaced", "sync", "held",
        "held.online", "error", "error.rateLimited", "error.busy", "error.draining", "restarting", "stillHere",
        "prompt.bet", "prompt.play", "prompt.bet-blind", "prompt.play-blind",
        "promptClosed.you", "promptClosed.computer",
        "game.gameStarted", "game.roundStarted", "game.betRequested", "game.betPlaced", "game.bettingComplete",
        "game.trickStarted", "game.cardRequested", "game.cardPlayed", "game.trickWon", "game.roundScored",
        "game.roundComplete", "game.gameOver", "game.gameStopped"};

    for(const std::string& name : expected)
    {
        INFO(name);
        REQUIRE(samples.contains(name));
        const Json::Value& message = samples.at(name);
        CHECK(test::shapeError(message).empty());
        const std::string difference = test::checkFixture(name, message);
        CHECK(difference.empty());
    }
    CHECK(samples.size() == expected.size());
}
