#include "core/Codec.h"
#include "core/Service.h"

#include "support/Client.h"
#include "support/Games.h"
#include "support/Recorder.h"
#include "support/ScriptedHuman.h"
#include "support/ServerMessages.h"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <future>
#include <thread>

using namespace whist;
using std::chrono::milliseconds;

namespace
{

const milliseconds patience(60000 * test::slowdown);

// Long enough for a game thread that is not held to send something.
const milliseconds quiet(300 * test::slowdown);

using Match = std::function<bool(const Json::Value&)>;

Match gameEvent(std::string kind)
{
    return [kind](const Json::Value& m) { return m["type"] == "game" && m["event"]["kind"] == kind; };
}

Match ofType(std::string type)
{
    return [type](const Json::Value& m) { return m["type"] == type; };
}

// One player at a solo table, through a session, as a browser plays.
struct Solo
{
    test::Recorder recorder;
    std::shared_ptr<Service> service;
    std::unique_ptr<test::Client> client;
    std::string token;

    explicit Solo(milliseconds instantFloor = milliseconds(0))
    {
        ServiceOptions options;
        options.registry.table.instantFloor = instantFloor;
        options.registry.table.threadNiceness = 0;
        service = std::make_shared<Service>(std::move(options));
        client = std::make_unique<test::Client>(service, recorder, 1);
        token = client->hello()["token"].asString();
    }

    ~Solo()
    {
        service->shutdown();
    }

    void create(const Settings& settings)
    {
        Json::Value message = test::typed("createSolo");
        message["settings"] = encodeSettings(settings);
        client->send(message);
    }

    void send(std::string_view type)
    {
        client->send(test::typed(type));
    }

    void updateSettings(const Settings& settings)
    {
        Json::Value message = test::typed("updateSettings");
        message["settings"] = encodeSettings(settings);
        client->send(message);
    }

    void continueRound(unsigned int round)
    {
        client->continuer()(round);
    }

    // The tab is reloaded: the old connection closes and a new one says hello with the token.
    // Returns the new connection's sync.
    Json::Value reload()
    {
        client->drop();
        const MemberId label = client->label() + 1;
        client = std::make_unique<test::Client>(service, recorder, label);
        client->hello(token);
        return *recorder.waitFor(label, ofType("sync"), patience);
    }

    MemberId label() const
    {
        return client->label();
    }

    std::optional<Json::Value> waitFor(const Match& match)
    {
        return recorder.waitFor(label(), match, patience);
    }

    std::vector<Json::Value> messages() const
    {
        return recorder.messagesTo(client->label());
    }

    // The messages after the one at `from` that match.
    std::size_t count(const Match& match, std::size_t from = 0) const
    {
        const auto all = messages();
        std::size_t found = 0;
        for(std::size_t i = from ; i < all.size() ; i++)
        {
            if(match(all[i]))
                found++;
        }
        return found;
    }
};

void checkShapes(const std::vector<Json::Value>& messages)
{
    for(const Json::Value& message : messages)
    {
        INFO(message.toStyledString());
        CHECK(test::shapeError(message).empty());
    }
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

TEST_CASE("A solo game is played to the end through a session, and again at once", "[solo]")
{
    Solo solo;
    solo.create(test::tableSettings(3, {1}));
    test::ScriptedHuman human(solo.recorder, solo.label(), solo.client->answerer(), test::Behaviour::Good);
    REQUIRE(human.waitUntilOver(patience));
    CHECK(human.problems().empty());
    CHECK(human.counts().closedByYou == human.counts().prompts);

    // Play again the moment the game is over.
    solo.send("startGame");
    REQUIRE(solo.waitFor([](const Json::Value& m) { return m["type"] == "game" && m["game"] == 2; }));
    CHECK(solo.count(ofType("error")) == 0);
    checkShapes(solo.messages());
}

TEST_CASE("A reload mid-trick resumes the game", "[solo][reload]")
{
    Solo solo;
    solo.create(test::tableSettings(4, {0}));

    // The human stops answering at a card prompt with cards already on the table.
    std::promise<Json::Value> stopped;
    std::atomic<bool> stopping = false;
    test::ScriptedHuman first(solo.recorder, solo.label(), solo.client->answerer(), test::Behaviour::Good,
        [&](const Json::Value& prompt)
        {
            if(stopping || prompt["kind"] != "play")
                return false;
            const auto messages = solo.recorder.messagesTo(1);
            for(auto m = messages.rbegin() ; m != messages.rend() ; ++m)
            {
                if((*m)["type"] != "game")
                    continue;
                if((*m)["snapshot"]["table"]["cards"].empty() || (*m)["snapshot"]["round"]["index"] < 3)
                    return false;
                stopping = true;
                stopped.set_value(prompt);
                return true;
            }
            return false;
        });

    auto waiting = stopped.get_future();
    REQUIRE(waiting.wait_for(patience) == std::future_status::ready);
    const Json::Value prompt = waiting.get();

    const Json::Value sync = solo.reload();
    checkShapes({sync});
    CHECK(sync["table"]["state"] == "playing");
    CHECK(sync["game"] == 1);
    CHECK_FALSE(sync["snapshot"]["table"]["cards"].empty());
    CHECK(sync["snapshot"]["active"] == 0);
    REQUIRE(sync["prompt"].isObject());
    CHECK(sync["prompt"]["id"] == prompt["id"]);
    CHECK(sync["prompt"]["legal"] == prompt["legal"]);
    CHECK(sync["history"].size() >= 3u);

    // The old tab hears nothing more.
    const std::size_t oldCount = solo.recorder.messagesTo(1).size();

    // The prompt from the sync is answered, and the game goes on to its end.
    test::ScriptedHuman second(solo.recorder, solo.label(), solo.client->answerer(), test::Behaviour::Good);
    solo.client->send(goodAnswer(sync["prompt"]));
    REQUIRE(second.waitUntilOver(patience));
    CHECK(second.problems().empty());
    CHECK(solo.recorder.messagesTo(1).size() == oldCount);
    CHECK(solo.count(gameEvent("gameOver")) == 1);
}

TEST_CASE("A watched game of computer players runs to the end, and a watcher who reloads picks it up", "[solo][watch]")
{
    Solo solo(milliseconds(1));
    solo.create(test::tableSettings(4, {}, Level::Cyborg));
    REQUIRE(solo.waitFor(gameEvent("roundScored")));

    const Json::Value sync = solo.reload();
    checkShapes({sync});
    CHECK(sync["table"]["host"].isNull());
    CHECK(sync["snapshot"]["you"].isNull());
    CHECK(sync["prompt"].isNull());
    CHECK(sync["history"].size() >= 1u);

    REQUIRE(solo.waitFor(gameEvent("gameOver")));
    checkShapes(solo.messages());
}

TEST_CASE("Pause holds the game at its next rest, and resume lets it go on", "[solo][pause]")
{
    Solo solo;
    solo.create(test::tableSettings(3, {0}));

    // Paused while the game waits on the human, who then answers.
    const auto prompt = solo.waitFor(ofType("prompt"));
    REQUIRE(prompt);
    solo.send("pause");
    REQUIRE(solo.waitFor([](const Json::Value& m) { return m["type"] == "tableState" && m["paused"] == true; }));
    solo.client->send(goodAnswer(*prompt));

    // The game goes as far as its next rest and holds there, even at Instant.
    REQUIRE(solo.waitFor(ofType("promptClosed")));
    std::this_thread::sleep_for(quiet);
    const std::size_t held = solo.messages().size();
    std::this_thread::sleep_for(quiet);
    CHECK(solo.messages().size() == held);
    CHECK(solo.count(ofType("prompt"), held) == 0);

    // Pausing twice changes nothing; resuming lets it go on.
    solo.send("pause");
    solo.send("resume");
    test::ScriptedHuman human(solo.recorder, solo.label(), solo.client->answerer(), test::Behaviour::Good);
    REQUIRE(human.waitUntilOver(patience));
    CHECK(solo.count([](const Json::Value& m) { return m["type"] == "tableState" && m["paused"] == false; }) >= 1);

    // The scripted human, started late, answers the prompt already answered by hand as well.
    CHECK(solo.count([](const Json::Value& m) { return m["type"] == "error" && m["code"] != "stalePrompt"; }) == 0);

    // A game that is over cannot be paused.
    solo.send("pause");
    CHECK(solo.waitFor([](const Json::Value& m) { return m["type"] == "error" && m["code"] == "illegal"; }));
}

TEST_CASE("With the wait on, every scored round holds until Continue", "[solo][hold]")
{
    Solo solo;
    Settings settings = test::tableSettings(2, {0});
    settings.waitAfterRound = true;
    solo.create(settings);
    test::ScriptedHuman human(solo.recorder, solo.label(), solo.client->answerer(), test::Behaviour::Good, {},
                              solo.client->continuer());
    REQUIRE(human.waitUntilOver(patience));
    CHECK(human.problems().empty());

    const auto messages = solo.messages();
    std::size_t scored = 0;
    for(std::size_t i = 0 ; i < messages.size() ; i++)
    {
        if(messages[i]["type"] != "game" || messages[i]["event"]["kind"] != "roundScored")
            continue;
        scored++;
        REQUIRE(i + 2 < messages.size());
        const Json::Value& held = messages[i + 1];
        CHECK(held["type"] == "held");
        CHECK(held["round"] == messages[i]["event"]["result"]["index"]);
        CHECK(held["waitingFor"] == test::parse("[0]"));
        CHECK(held["deadline"].isNull());
        CHECK(messages[i + 2]["event"]["kind"] == "roundComplete");
    }
    CHECK(scored == messages.back()["snapshot"]["round"]["count"].asUInt());
    CHECK(human.counts().holds == scored);
    checkShapes(messages);
}

TEST_CASE("A hold: a stale Continue does nothing, a sync carries it, and Continue while paused ends it",
          "[solo][hold]")
{
    Solo solo;
    Settings settings = test::tableSettings(3, {0});
    settings.waitAfterRound = true;
    solo.create(settings);
    test::ScriptedHuman human(solo.recorder, solo.label(), solo.client->answerer(), test::Behaviour::Good);

    const auto held = solo.waitFor(ofType("held"));
    REQUIRE(held);
    const unsigned int round = (*held)["round"].asUInt();

    solo.continueRound(round + 1);
    std::this_thread::sleep_for(quiet);
    CHECK(solo.messages().back()["type"] == "held");

    // A reload during the hold finds it in the sync.
    Json::Value sync = solo.reload();
    REQUIRE(sync["held"].isObject());
    CHECK(sync["held"]["round"].asUInt() == round);
    CHECK(sync["held"]["deadline"].isNull());

    // Continue while paused: the hold is over, though the game stays paused.
    solo.send("pause");
    solo.continueRound(round);
    std::this_thread::sleep_for(quiet);
    CHECK(solo.count(gameEvent("roundComplete")) == 0);
    sync = solo.reload();
    CHECK(sync["held"].isNull());
    CHECK(sync["table"]["paused"] == true);

    solo.send("resume");
    REQUIRE(solo.waitFor(gameEvent("roundComplete")));
}

TEST_CASE("The wait after each round, turned on and off mid-game, applies from the next round", "[solo][hold]")
{
    Solo solo;
    Settings settings = test::tableSettings(2, {0});
    solo.create(settings);

    // Turned on while the human thinks about their first move, so the first round already waits.
    std::atomic<bool> turnedOn = false;
    test::ScriptedHuman human(solo.recorder, solo.label(), solo.client->answerer(), test::Behaviour::Good,
        [&](const Json::Value&)
        {
            if(!turnedOn.exchange(true))
            {
                Settings waiting = settings;
                waiting.waitAfterRound = true;
                solo.updateSettings(waiting);
            }
            return false;
        });

    const auto held = solo.waitFor(ofType("held"));
    REQUIRE(held);
    CHECK((*held)["round"] == 0);

    // Turned off during the hold: this hold stays, and no later round waits.
    solo.updateSettings(settings);
    std::this_thread::sleep_for(quiet);
    CHECK(solo.count(gameEvent("roundComplete")) == 0);

    solo.continueRound(0);
    REQUIRE(human.waitUntilOver(patience));
    CHECK(solo.count(ofType("held")) == 1);
    CHECK(solo.count(ofType("error")) == 0);
}

TEST_CASE("A new speed applies to the beat under way", "[solo][speed]")
{
    Solo solo;
    Settings settings = test::tableSettings(3, {0});
    settings.botSpeed = slowestSpeed;
    solo.create(settings);
    test::ScriptedHuman human(solo.recorder, solo.label(), solo.client->answerer(), test::Behaviour::Good);

    // A computer is about to play its card, after a 1400 ms beat.
    const auto requested = solo.waitFor([](const Json::Value& m)
    {
        return m["type"] == "game" && m["event"]["kind"] == "cardRequested" && m["event"]["seat"] != 0;
    });
    REQUIRE(requested);
    const auto changed = Clock::now();
    settings.botSpeed = fastestSpeed;
    solo.updateSettings(settings);

    const Json::Value seat = (*requested)["event"]["seat"];
    REQUIRE(solo.waitFor([&](const Json::Value& m)
    {
        return m["type"] == "game" && m["event"]["kind"] == "cardPlayed" && m["event"]["seat"] == seat;
    }));
    CHECK(Clock::now() - changed < milliseconds(700));
    CHECK(solo.count([](const Json::Value& m) { return m["type"] == "tableState" && m["settings"]["botSpeed"] == fastestSpeed; }) == 1);
}

TEST_CASE("Mid-game, only the speed and the wait may change", "[solo][settings]")
{
    Solo solo;
    Settings settings = test::tableSettings(3, {0});
    solo.create(settings);
    REQUIRE(solo.waitFor(ofType("prompt")));

    Settings shape = settings;
    shape.structure = romanian_whist::GameStructure::S_818;
    solo.updateSettings(shape);
    Settings renamed = settings;
    renamed.seats[1].profile.name = "Zoe";
    solo.updateSettings(renamed);
    Settings bad = settings;
    bad.seats.clear();
    solo.updateSettings(bad);

    const auto messages = solo.messages();
    REQUIRE(solo.count(ofType("error")) == 3);
    std::vector<std::string> codes;
    for(const Json::Value& m : messages)
    {
        if(m["type"] == "error")
            codes.push_back(m["code"].asString());
    }
    CHECK(codes == std::vector<std::string>{"illegal", "illegal", "badMessage"});

    settings.botSpeed = 1;
    solo.updateSettings(settings);
    REQUIRE(solo.waitFor([](const Json::Value& m) { return m["type"] == "tableState" && m["settings"]["botSpeed"] == 1; }));
}

TEST_CASE("Leaving a solo game closes it, and nothing of it follows into the next", "[solo][leave]")
{
    Solo solo;
    solo.create(test::tableSettings(3, {0}));
    REQUIRE(solo.waitFor(ofType("prompt")));

    solo.send("leaveTable");
    REQUIRE(solo.waitFor([](const Json::Value& m) { return m["type"] == "tableState" && m["state"] == "closed"; }));
    std::this_thread::sleep_for(quiet);
    const auto closed = solo.messages();
    CHECK(closed.back()["type"] == "tableState");
    CHECK(closed.back()["state"] == "closed");
    CHECK(solo.service->registry().tableCount() == 0);

    // Not at a table any more: leaving again is refused, and a new game can start.
    solo.send("leaveTable");
    solo.create(test::tableSettings(3, {}));
    REQUIRE(solo.waitFor(gameEvent("gameOver")));
    const auto all = solo.messages();
    CHECK(all[closed.size()]["code"] == "illegal");
    CHECK(all[closed.size() + 1]["type"] == "tableState");
    CHECK(all[closed.size() + 1]["code"] != closed.back()["code"]);
    CHECK(all[closed.size() + 1]["state"] == "playing");
    checkShapes(all);
}
