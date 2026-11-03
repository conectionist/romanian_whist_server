#include "core/Codec.h"
#include "core/GameRun.h"

#include "support/Games.h"
#include "support/Recorder.h"
#include "support/ScriptedHuman.h"
#include "support/ServerMessages.h"

#include <catch2/catch_test_macros.hpp>

#include <memory>

using namespace whist;
using std::chrono::milliseconds;

namespace
{

constexpr MemberId ana = 1;
constexpr MemberId bogdan = 2;
constexpr MemberId watcher = 9;
const milliseconds patience(60000 * test::slowdown);

struct Game
{
    test::Recorder recorder;
    Outbox outbox;
    std::unique_ptr<GameRun> run;

    explicit Game(GameRunSetup setup)
    {
        for(MemberId member : {ana, bogdan, watcher})
            outbox.attach(member, recorder.connect(member));
        for(MemberId member : setup.watchers)
            outbox.attach(member, recorder.connect(member));
        run = std::make_unique<GameRun>(std::move(setup), outbox);
    }

    // Answers as Table::answer does from an I/O thread, error replies included.
    test::ScriptedHuman::Answerer answerer(unsigned int seat, MemberId member)
    {
        return [this, seat, member](const Answer& answer)
        {
            const SeatDriver::Submitted submitted = run->submit(seat, answer);
            if(submitted == SeatDriver::Submitted::Stale)
                outbox.send(member, errorMessage(ErrorCode::StalePrompt));
            else if(submitted == SeatDriver::Submitted::NotYourTurn)
                outbox.send(member, errorMessage(ErrorCode::NotYourTurn));
        };
    }

    // Closes the game, then times how long its thread takes to join.
    milliseconds closeAndJoin()
    {
        const auto started = Clock::now();
        run->close();
        run.reset();
        return std::chrono::duration_cast<milliseconds>(Clock::now() - started);
    }

    bool waitFor(MemberId member, const std::string& eventKind)
    {
        return recorder.waitFor(member, [&](const Json::Value& m)
        {
            return m["type"] == "game" && m["event"]["kind"] == eventKind;
        }, patience).has_value();
    }
};

void checkEveryMessage(const test::Recorder& recorder, MemberId member)
{
    for(const Json::Value& message : recorder.messagesTo(member))
    {
        const std::string error = test::shapeError(message);
        INFO(message.toStyledString());
        CHECK(error.empty());
    }
}

} // namespace

TEST_CASE("A solo game runs to the end with a scripted human", "[game]")
{
    Settings settings = test::tableSettings(3, {0}, Level::Random);
    Game game(test::quickGame(settings, {ana}));
    test::ScriptedHuman human(game.recorder, ana, game.answerer(0, ana), test::Behaviour::Good);
    game.run->start();

    REQUIRE(human.waitUntilOver(patience));
    game.run.reset();

    CHECK(human.problems().empty());
    const auto counts = human.counts();
    CHECK(counts.prompts > 0);
    CHECK(counts.closedByYou == counts.prompts);
    CHECK(counts.closedByComputer == 0);
    CHECK(counts.illegal == 0);

    const auto messages = game.recorder.messagesTo(ana);
    REQUIRE_FALSE(messages.empty());
    const Json::Value& last = messages.back();
    CHECK(last["event"]["kind"] == "gameOver");
    CHECK(last["snapshot"]["status"] == "finished");
    CHECK(last["event"]["standings"].size() == 3);
    CHECK(last["event"]["winners"].size() >= 1);

    // Seq numbers the member's messages without a gap.
    for(std::size_t i = 0 ; i < messages.size() ; i++)
        CHECK(messages[i]["seq"].asUInt64() == i + 1);
    checkEveryMessage(game.recorder, ana);
}

TEST_CASE("Bad answers are refused and re-prompted, and the game goes on", "[game]")
{
    Settings settings = test::tableSettings(4, {1}, Level::LowRisk);
    Game game(test::quickGame(settings, {std::nullopt, ana}, 21));
    test::ScriptedHuman human(game.recorder, ana, game.answerer(1, ana), test::Behaviour::Garbage);
    game.run->start();

    REQUIRE(human.waitUntilOver(patience));
    REQUIRE(game.waitFor(ana, "gameOver"));
    CHECK(human.problems().empty());

    // Each prompt: one stale answer, one illegal answer and its re-prompt, then the answer twice.
    const auto counts = human.counts();
    CHECK(counts.prompts > 0);
    CHECK(counts.illegal == counts.prompts);
    CHECK(counts.closedByYou == counts.prompts);
    CHECK(counts.stale == 2 * counts.prompts);
    checkEveryMessage(game.recorder, ana);
}

TEST_CASE("Closing a game joins its thread at once", "[game][close]")
{
    const milliseconds bound(1000 * test::slowdown);

    SECTION("while a human is thinking")
    {
        Settings settings = test::tableSettings(3, {0});
        Game game(test::quickGame(settings, {ana}));
        test::ScriptedHuman human(game.recorder, ana, game.answerer(0, ana), test::Behaviour::Silent);
        game.run->start();
        REQUIRE(human.waitForPrompt(patience));

        game.run->close();
        while(!game.run->finished())
            std::this_thread::sleep_for(milliseconds(1));
        CHECK(game.run->outcome() == RunOutcome::Abandoned);
        CHECK(game.closeAndJoin() < bound);
    }

    SECTION("while resting on a finished trick")
    {
        Settings settings = test::tableSettings(3, {});
        settings.botSpeed = slowestSpeed;
        GameRunSetup setup = test::quickGame(settings, {});
        setup.watchers = {watcher};
        Game game(std::move(setup));
        game.run->start();
        REQUIRE(game.waitFor(watcher, "trickWon"));

        CHECK(game.closeAndJoin() < bound);
        CHECK(game.recorder.messagesTo(watcher).back()["event"]["kind"] == "gameStopped");
    }

    SECTION("while a Reckoner thinks")
    {
        Settings settings = test::tableSettings(6, {}, Level::ReckonerGamma);
        GameRunSetup setup = test::quickGame(settings, {});
        setup.watchers = {watcher};
        Game game(std::move(setup));
        game.run->start();
        REQUIRE(game.waitFor(watcher, "betRequested"));

        CHECK(game.closeAndJoin() < bound);
    }

    SECTION("before it ever started")
    {
        Game game(test::quickGame(test::tableSettings(3, {0}), {ana}));
        CHECK(game.closeAndJoin() < bound);
    }

    SECTION("after it finished")
    {
        Game game(test::quickGame(test::tableSettings(2, {}), {}));
        game.run->start();
        while(!game.run->finished())
            std::this_thread::sleep_for(milliseconds(1));
        CHECK(game.run->outcome() == RunOutcome::Finished);
        CHECK(game.closeAndJoin() < bound);
    }
}

TEST_CASE("Online, a turn that times out is played by the seat's computer player", "[game][timer]")
{
    Settings settings = test::tableSettings(3, {0, 1});
    settings.replacement = Level::Cyborg;
    settings.turnTimer = milliseconds(3);
    REQUIRE_FALSE(checkSettings(settings, Mode::Online));

    Game game(test::quickGame(settings, {ana, bogdan}));
    test::ScriptedHuman silent(game.recorder, ana, game.answerer(0, ana), test::Behaviour::Silent);
    test::ScriptedHuman good(game.recorder, bogdan, game.answerer(1, bogdan), test::Behaviour::Good);
    game.run->start();

    REQUIRE(silent.waitUntilOver(patience));
    REQUIRE(good.waitUntilOver(patience));
    game.run.reset();

    const auto missed = silent.counts();
    CHECK(missed.prompts > 0);
    CHECK(missed.closedByComputer == missed.prompts);
    CHECK(missed.closedByYou == 0);
    CHECK(silent.problems().empty());

    // The other human answers in time, at least nearly always.
    const auto answered = good.counts();
    CHECK(answered.closedByYou + answered.closedByComputer == answered.prompts);
    CHECK(good.problems().empty());
}

TEST_CASE("Each viewer gets their own snapshot, and a watcher sees no hand", "[game][publisher]")
{
    Settings settings = test::tableSettings(3, {0, 2});
    GameRunSetup setup = test::quickGame(settings, {ana, std::nullopt, bogdan});
    setup.watchers = {watcher};
    Game game(std::move(setup));
    test::ScriptedHuman first(game.recorder, ana, game.answerer(0, ana), test::Behaviour::Good);
    test::ScriptedHuman second(game.recorder, bogdan, game.answerer(2, bogdan), test::Behaviour::Good);
    game.run->start();
    REQUIRE(first.waitUntilOver(patience));
    REQUIRE(second.waitUntilOver(patience));
    REQUIRE(game.waitFor(watcher, "gameOver"));

    for(const auto& [member, seat] : {std::pair{ana, 0}, std::pair{bogdan, 2}})
    {
        for(const Json::Value& message : game.recorder.messagesTo(member))
        {
            if(message["type"] != "game")
                continue;
            CHECK(message["snapshot"]["you"] == seat);
            const Json::Value& round = message["snapshot"]["round"];
            const bool normal = round["type"] == "normal";
            for(unsigned int s = 0 ; s < 3 ; s++)
            {
                const bool hasHand = message["snapshot"]["seats"][s].isMember("hand");
                if(normal)
                    CHECK(hasHand == (static_cast<int>(s) == seat));
                else if(round["type"] == "hidden")
                    CHECK_FALSE(hasHand);
            }
        }
    }

    for(const Json::Value& message : game.recorder.messagesTo(watcher))
    {
        CHECK(message["snapshot"]["you"].isNull());
        for(const Json::Value& seat : message["snapshot"]["seats"])
            CHECK_FALSE(seat.isMember("hand"));
    }

    // The publisher's cache kept a record of every round, and the last trick.
    game.run.reset();
}

TEST_CASE("The publisher keeps every scored round and the last trick across rounds", "[game][publisher]")
{
    Game game(test::quickGame(test::tableSettings(3, {}), {}));
    game.run->start();
    while(!game.run->finished())
        std::this_thread::sleep_for(milliseconds(1));

    const std::vector<RoundResult> history = game.run->publisher().history();
    REQUIRE(history.size() == 23);
    for(unsigned int i = 0 ; i < history.size() ; i++)
    {
        CHECK(history[i].index == i);
        CHECK(history[i].seats.size() == 3);
    }
    CHECK(history.back().type == romanian_whist::RoundType::Hidden);
    CHECK_FALSE(history.back().nextCountsForStreaks);

    const std::optional<LastTrick> last = game.run->publisher().lastTrick();
    REQUIRE(last);
    CHECK(last->round == 22);
    CHECK(last->cards.size() == 3);
    CHECK(last->winning);
}
