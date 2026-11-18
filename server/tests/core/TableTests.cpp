#include "core/Codec.h"
#include "core/Registry.h"

#include "support/Games.h"
#include "support/Recorder.h"
#include "support/ScriptedHuman.h"
#include "support/ServerMessages.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <random>
#include <set>

using namespace whist;
using std::chrono::milliseconds;

namespace
{

const milliseconds patience(60000 * test::slowdown);

RegistryOptions quickTables()
{
    RegistryOptions options;
    options.table.instantFloor = milliseconds(0);
    options.table.threadNiceness = 0;
    return options;
}

bool waitUntil(const std::function<bool()>& done)
{
    const auto deadline = Clock::now() + patience;
    while(!done())
    {
        if(Clock::now() > deadline)
            return false;
        std::this_thread::sleep_for(milliseconds(1));
    }
    return true;
}

} // namespace

TEST_CASE("Tokens are 128 random bits, and table codes avoid look-alikes", "[registry]")
{
    test::Recorder recorder;
    Outbox outbox;
    Registry registry(outbox, quickTables());

    std::set<std::string> tokens;
    for(int i = 0 ; i < 1000 ; i++)
    {
        const Registry::NewMember member = registry.newMember();
        REQUIRE(member.token.size() == 22);
        CHECK(member.token.find_first_not_of(
                  "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_") == std::string::npos);
        CHECK(tokens.insert(member.token).second);
        CHECK(registry.findMember(member.token) == member.id);
    }
    CHECK_FALSE(registry.findMember("AAAAAAAAAAAAAAAAAAAAAA"));

    std::set<std::string> codes;
    for(int i = 0 ; i < 50 ; i++)
    {
        const Registry::NewMember member = registry.newMember();
        Settings settings = test::tableSettings(2, {});
        const Registry::Created created = registry.createSolo(member.id, settings);
        REQUIRE(created.table);
        const std::string& code = created.table->code();
        CHECK(code.size() == 6);
        CHECK(code.find_first_not_of("ABCDEFGHJKMNPQRSTUVWXYZ23456789") == std::string::npos);
        CHECK(codes.insert(code).second);
        CHECK(registry.findTable(code) == created.table);
        CHECK(registry.tableOf(member.id) == created.table);
    }
    registry.shutdown();
    CHECK(registry.tableCount() == 0);
}

TEST_CASE("Creating a solo table checks its settings and its member", "[registry]")
{
    Outbox outbox;
    Registry registry(outbox, quickTables());
    const Registry::NewMember member = registry.newMember();

    const Registry::Created bad = registry.createSolo(member.id, test::tableSettings(3, {0, 1}));
    CHECK_FALSE(bad.table);
    CHECK(bad.error == ErrorCode::BadMessage);
    CHECK(bad.detail == "seats");

    const Registry::Created nobody = registry.createSolo(999, test::tableSettings(3, {0}));
    CHECK(nobody.error == ErrorCode::NotFound);

    REQUIRE(registry.createSolo(member.id, test::tableSettings(3, {})).table);
    const Registry::Created twice = registry.createSolo(member.id, test::tableSettings(3, {}));
    CHECK(twice.error == ErrorCode::Illegal);

    registry.shutdown();
    const Registry::NewMember late = registry.newMember();
    CHECK(registry.createSolo(late.id, test::tableSettings(3, {})).error == ErrorCode::Draining);
}

TEST_CASE("A solo table plays, finishes, plays again and closes", "[table]")
{
    test::Recorder recorder;
    Outbox outbox;
    Registry registry(outbox, quickTables());

    const Registry::NewMember ana = registry.newMember();
    outbox.attach(ana.id, recorder.connect(ana.id));

    const std::shared_ptr<Table> table = registry.createSolo(ana.id, test::tableSettings(3, {1})).table;
    REQUIRE(table);
    test::ScriptedHuman human(recorder, ana.id, [&](const Answer& answer) { table->answer(ana.id, answer); },
                              test::Behaviour::Good);

    const auto first = recorder.messagesTo(ana.id).front();
    CHECK(first["type"] == "tableState");
    CHECK(first["state"] == "playing");
    CHECK(first["mode"] == "solo");
    CHECK(first["host"] == 1);
    CHECK(first["seats"][1]["you"] == true);
    CHECK(first["seats"][1]["presence"] == "here");
    CHECK(first["seats"][0]["kind"] == "computer");
    CHECK(test::shapeError(first).empty());

    REQUIRE(human.waitUntilOver(patience));
    REQUIRE(waitUntil([&] { return table->state() == TableState::Finished; }));
    CHECK(table->outcome() == RunOutcome::Finished);

    // A stranger may not start the next game, and an answer now finds no prompt.
    CHECK(table->startGame(ana.id + 100) == ErrorCode::NotHost);
    CHECK(table->answer(ana.id, Answer{PromptKind::Bet, 1, 0}) == ErrorCode::StalePrompt);

    // A new game at the same table: the old one is joined off this thread.
    const std::size_t before = recorder.messagesTo(ana.id).size();
    CHECK_FALSE(table->startGame(ana.id));
    CHECK(table->game() == 2);
    CHECK(table->startGame(ana.id) == ErrorCode::Illegal);
    CHECK(waitUntil([&] { return registry.joiner().joined() == 1; }));
    CHECK(recorder.messagesTo(ana.id)[before]["type"] == "tableState");

    REQUIRE(recorder.waitFor(ana.id, [](const Json::Value& m) { return m["type"] == "game" && m["game"] == 2; }, patience));
    registry.closeTable(table->code());
    CHECK(table->state() == TableState::Closed);
    CHECK_FALSE(registry.findTable(table->code()));
    CHECK_FALSE(registry.tableOf(ana.id));
    CHECK(waitUntil([&] { return registry.joiner().joined() == 2; }));

    // Nothing of the game's follows the table's own word that it closed.
    const auto closing = recorder.messagesTo(ana.id);
    CHECK(closing.back()["type"] == "tableState");
    CHECK(closing.back()["state"] == "closed");
}

TEST_CASE("Watching a solo game: the member has no seat and sees no hand", "[table]")
{
    test::Recorder recorder;
    Outbox outbox;
    Registry registry(outbox, quickTables());
    const Registry::NewMember viewer = registry.newMember();
    outbox.attach(viewer.id, recorder.connect(viewer.id));

    const std::shared_ptr<Table> table = registry.createSolo(viewer.id, test::tableSettings(4, {})).table;
    REQUIRE(table);
    REQUIRE(recorder.waitFor(viewer.id, [](const Json::Value& m) { return m["event"]["kind"] == "gameOver"; }, patience));

    for(const Json::Value& message : recorder.messagesTo(viewer.id))
    {
        CHECK(test::shapeError(message).empty());
        if(message["type"] == "tableState")
            CHECK(message["host"].isNull());
        if(message["type"] == "game")
        {
            CHECK(message["snapshot"]["you"].isNull());
            for(const Json::Value& seat : message["snapshot"]["seats"])
                CHECK_FALSE(seat.isMember("hand"));
        }
    }
}

TEST_CASE("A sync carries what a fresh viewer would get", "[table][sync]")
{
    test::Recorder recorder;
    Outbox outbox;
    Registry registry(outbox, quickTables());
    const Registry::NewMember ana = registry.newMember();
    outbox.attach(ana.id, recorder.connect(ana.id));

    std::shared_ptr<Table> table;
    std::atomic<int> plays = 0;
    std::optional<Json::Value> syncedPrompt;

    // At the human's fortieth card, while the game waits for them, they reload.
    const auto onPrompt = [&](const Json::Value& prompt)
    {
        if(prompt["kind"] != "play" || ++plays != 40)
            return false;
        syncedPrompt = prompt;
        table->sync(ana.id);
        return false;
    };

    table = registry.createSolo(ana.id, test::tableSettings(3, {0})).table;
    REQUIRE(table);
    test::ScriptedHuman human(recorder, ana.id, [&](const Answer& answer) { table->answer(ana.id, answer); },
                              test::Behaviour::Good, onPrompt);
    REQUIRE(human.waitUntilOver(patience));
    REQUIRE(syncedPrompt);

    const auto messages = recorder.messagesTo(ana.id);
    std::optional<Json::Value> sync;
    std::optional<Json::Value> lastGame;
    std::optional<Json::Value> lastTrickWon;
    std::size_t roundsScored = 0;
    for(const Json::Value& message : messages)
    {
        if(message["type"] == "sync")
        {
            sync = message;
            break;
        }
        if(message["type"] == "game")
        {
            lastGame = message;
            if(message["event"]["kind"] == "roundScored")
                roundsScored++;
            if(message["event"]["kind"] == "trickWon")
                lastTrickWon = message;
        }
    }

    REQUIRE(sync);
    REQUIRE(lastGame);
    REQUIRE(lastTrickWon);
    CHECK(test::shapeError(*sync).empty());
    CHECK((*sync)["game"] == 1);
    CHECK((*sync)["table"]["state"] == "playing");
    CHECK((*sync)["snapshot"] == (*lastGame)["snapshot"]);
    CHECK((*sync)["history"].size() == roundsScored);

    Json::Value prompt = *syncedPrompt;
    prompt.removeMember("type");
    prompt.removeMember("seq");
    CHECK((*sync)["prompt"] == prompt);

    const Json::Value& trick = (*sync)["lastTrick"];
    CHECK(trick["cards"] == (*lastTrickWon)["snapshot"]["table"]["cards"]);
    CHECK(trick["round"] == (*lastTrickWon)["snapshot"]["round"]["index"]);
    CHECK((*sync)["held"].isNull());
}

TEST_CASE("Shutting the registry down joins every game", "[registry]")
{
    test::Recorder recorder;
    Outbox outbox;
    Registry registry(outbox, quickTables());

    std::vector<std::unique_ptr<test::ScriptedHuman>> humans;
    for(int i = 0 ; i < 5 ; i++)
    {
        const Registry::NewMember member = registry.newMember();
        outbox.attach(member.id, recorder.connect(member.id));
        const auto table = registry.createSolo(member.id, test::tableSettings(3, {0})).table;
        REQUIRE(table);
        humans.push_back(std::make_unique<test::ScriptedHuman>(recorder, member.id, [](const Answer&) {},
                                                               test::Behaviour::Silent));
        REQUIRE(humans.back()->waitForPrompt(patience));
    }

    const auto started = Clock::now();
    registry.shutdown();
    CHECK(Clock::now() - started < milliseconds(1000 * test::slowdown));
    CHECK(registry.joiner().joined() == 5);
}
