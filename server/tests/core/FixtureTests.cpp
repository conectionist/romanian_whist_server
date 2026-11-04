#include "core/Codec.h"
#include "core/GameRun.h"
#include "core/Registry.h"
#include "core/Service.h"

#include "support/Client.h"
#include "support/Fixtures.h"
#include "support/Games.h"
#include "support/Recorder.h"
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

} // namespace

TEST_CASE("Every message the server sends has a fixture, and it is up to date", "[fixtures]")
{
    Samples samples;
    soloGame(samples);
    timedOutGame(samples);
    stoppedGame(samples);

    const std::vector<std::string> expected{
        "welcome", "tableState", "sync", "held", "error",
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
