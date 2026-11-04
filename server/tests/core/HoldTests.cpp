#include "core/Codec.h"
#include "core/GameRun.h"

#include "support/Games.h"
#include "support/Recorder.h"
#include "support/ScriptedHuman.h"
#include "support/ServerMessages.h"

#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <thread>

using namespace whist;
using std::chrono::milliseconds;

namespace
{

const milliseconds patience(60000 * test::slowdown);

// Far longer than a game takes when every hold is ended by its players' Continues.
const milliseconds longHold(20000 * test::slowdown);

using Match = std::function<bool(const Json::Value&)>;

Match ofType(std::string type)
{
    return [type](const Json::Value& m) { return m["type"] == type; };
}

std::vector<unsigned int> seats(const Json::Value& list)
{
    std::vector<unsigned int> result;
    for(const Json::Value& seat : list)
        result.push_back(seat.asUInt());
    return result;
}

// An online game of humans at seats 0 to humans - 1, member seat + 1, and computer players after
// them, waiting after each round. Members in `away` have no connection.
struct OnlineGame
{
    test::Recorder recorder;
    Outbox outbox;
    std::unique_ptr<GameRun> run;
    std::vector<std::unique_ptr<test::ScriptedHuman>> humans;

    OnlineGame(unsigned int seatCount, unsigned int humanCount, milliseconds hold, const std::vector<MemberId>& away = {})
    {
        std::vector<unsigned int> humanSeats;
        std::vector<std::optional<MemberId>> members;
        for(unsigned int seat = 0 ; seat < humanCount ; seat++)
        {
            humanSeats.push_back(seat);
            members.push_back(seat + 1);
            if(std::ranges::find(away, seat + 1) == away.end())
                outbox.attach(seat + 1, recorder.connect(seat + 1));
        }

        Settings settings = test::tableSettings(seatCount, humanSeats);
        settings.structure = romanian_whist::GameStructure::S_818;
        settings.foreheadAndHidden = false;
        settings.waitAfterRound = true;
        settings.replacement = Level::Random;
        if(!away.empty())
            settings.turnTimer = milliseconds(1);

        GameRunSetup setup = test::quickGame(settings, members);
        setup.roundHold = hold;
        run = std::make_unique<GameRun>(std::move(setup), outbox);
    }

    ~OnlineGame()
    {
        if(run)
            run->close();
        humans.clear();
    }

    // A player who answers every prompt, and, when `continues`, every hold that awaits them.
    void addHuman(MemberId member, bool continues)
    {
        test::ScriptedHuman::Continuer continuer;
        if(continues)
            continuer = [this, member](unsigned int round) { run->continueRound(member, round); };
        humans.push_back(std::make_unique<test::ScriptedHuman>(
            recorder, member, [this, member](const Answer& answer) { run->submit(member, answer); },
            test::Behaviour::Good, test::ScriptedHuman::OnPrompt{}, continuer));
    }

    bool finish()
    {
        for(const auto& human : humans)
        {
            if(!human->waitUntilOver(patience))
                return false;
        }
        const auto deadline = Clock::now() + patience;
        while(!run->finished() && Clock::now() < deadline)
            std::this_thread::sleep_for(milliseconds(1));
        return run->outcome() == RunOutcome::Finished;
    }

    std::vector<Json::Value> held(MemberId member) const
    {
        std::vector<Json::Value> found;
        for(const Json::Value& message : recorder.messagesTo(member))
        {
            if(message["type"] == "held")
                found.push_back(message);
        }
        return found;
    }
};

// No hold of a round is sent once that round is complete.
void checkNoHoldAfterItsRound(const std::vector<Json::Value>& messages)
{
    std::optional<unsigned int> scored;
    std::optional<unsigned int> complete;
    for(const Json::Value& message : messages)
    {
        if(message["type"] == "game" && message["event"]["kind"] == "roundScored")
            scored = message["event"]["result"]["index"].asUInt();
        else if(message["type"] == "game" && message["event"]["kind"] == "roundComplete")
            complete = scored;
        else if(message["type"] == "held")
        {
            INFO(message.toStyledString());
            CHECK(test::shapeError(message).empty());
            CHECK((!complete || message["round"].asUInt() > *complete));
        }
    }
}

} // namespace

TEST_CASE("Online, the hold ends once every connected human continues", "[hold][online]")
{
    for(int game = 0 ; game < 3 ; game++)
    {
        OnlineGame online(4, 3, longHold);
        for(MemberId member : {1, 2, 3})
            online.addHuman(member, true);
        online.run->start();
        REQUIRE(online.finish());

        const auto rounds = online.run->publisher().history().size();
        for(MemberId member : {1, 2, 3})
        {
            const std::vector<Json::Value> messages = online.recorder.messagesTo(member);
            checkNoHoldAfterItsRound(messages);

            std::set<unsigned int> heldRounds;
            for(const Json::Value& held : online.held(member))
            {
                heldRounds.insert(held["round"].asUInt());
                CHECK(held["waitingFor"].size() >= 1);
                CHECK(held["deadline"].asInt64() <= longHold.count());
            }
            CHECK(heldRounds.size() == rounds);
        }
    }
}

TEST_CASE("Online, the hold ends at its deadline for a human who does not continue", "[hold][online]")
{
    const milliseconds hold(300 * test::slowdown);
    OnlineGame online(3, 2, hold);
    online.addHuman(1, false);
    online.addHuman(2, true);
    online.run->start();
    REQUIRE(online.finish());

    // Seat 1 continued each time, so each hold was sent again awaiting seat 0 alone.
    std::size_t again = 0;
    for(const Json::Value& held : online.held(1))
    {
        CHECK(held["deadline"].asInt64() <= hold.count());
        if(seats(held["waitingFor"]) == std::vector<unsigned int>{0})
            again++;
    }
    CHECK(again == online.run->publisher().history().size());
    checkNoHoldAfterItsRound(online.recorder.messagesTo(1));
}

TEST_CASE("Online, a human who is not connected is not waited for", "[hold][online]")
{
    OnlineGame online(3, 2, longHold, {2});
    online.addHuman(1, true);
    online.run->start();
    REQUIRE(online.finish());

    const std::vector<Json::Value> held = online.held(1);
    CHECK(held.size() == online.run->publisher().history().size());
    for(const Json::Value& message : held)
        CHECK(seats(message["waitingFor"]) == std::vector<unsigned int>{0});
}

TEST_CASE("Online, a Continue sent on seeing the round's result counts", "[hold][online]")
{
    OnlineGame online(3, 1, longHold);
    online.addHuman(1, false);

    std::atomic<bool> done{false};
    std::thread reader([&]
    {
        std::size_t cursor = 0;
        while(!done)
        {
            const std::optional<Json::Value> message = online.recorder.next(1, cursor, milliseconds(20));
            if(message && (*message)["event"]["kind"] == "roundScored")
                online.run->continueRound(1, (*message)["event"]["result"]["index"].asUInt());
        }
    });

    online.run->start();
    const bool finished = online.finish();
    done = true;
    reader.join();
    CHECK(finished);
}

TEST_CASE("Online, a late Continue leaves the next round's hold alone", "[hold][online]")
{
    OnlineGame online(3, 2, milliseconds(300 * test::slowdown));
    online.addHuman(2, true);

    // Seat 0 continues only the round before, each time.
    online.humans.push_back(std::make_unique<test::ScriptedHuman>(
        online.recorder, 1, [&online](const Answer& answer) { online.run->submit(1, answer); }, test::Behaviour::Good,
        test::ScriptedHuman::OnPrompt{}, [&online](unsigned int round)
        {
            if(round > 0)
                online.run->continueRound(1, round - 1);
        }));
    online.run->start();
    REQUIRE(online.finish());

    // Every hold was sent again awaiting seat 0, who never continued it.
    std::set<unsigned int> stillAwaited;
    for(const Json::Value& held : online.held(2))
    {
        if(seats(held["waitingFor"]) == std::vector<unsigned int>{0})
            stillAwaited.insert(held["round"].asUInt());
    }
    CHECK(stillAwaited.size() == online.run->publisher().history().size());
}

TEST_CASE("Online, a sync during the hold shows who is awaited and the time left", "[hold][online][sync]")
{
    const milliseconds hold(5000 * test::slowdown);
    OnlineGame online(3, 2, hold);
    online.addHuman(1, false);
    online.addHuman(2, false);
    online.run->start();

    const auto first = online.recorder.waitFor(1, ofType("held"), patience);
    REQUIRE(first);
    const unsigned int round = (*first)["round"].asUInt();
    CHECK(seats((*first)["waitingFor"]) == std::vector<unsigned int>{0, 1});

    online.run->continueRound(2, round);
    REQUIRE(online.recorder.waitFor(1, [](const Json::Value& m)
    {
        return m["type"] == "held" && m["waitingFor"].size() == 1;
    }, patience));

    std::this_thread::sleep_for(milliseconds(20));
    online.run->sendSync(1, TableView{});
    const auto sync = online.recorder.waitFor(1, ofType("sync"), patience);
    REQUIRE(sync);
    const Json::Value& held = (*sync)["held"];
    REQUIRE(held.isObject());
    CHECK(held["round"].asUInt() == round);
    CHECK(seats(held["waitingFor"]) == std::vector<unsigned int>{0});
    CHECK(held["deadline"].asInt64() > 0);
    CHECK(held["deadline"].asInt64() < hold.count());

    // The seat's human leaves, so the hold waits for nobody and ends.
    online.run->excuse(1);
    REQUIRE(online.recorder.waitFor(1, [](const Json::Value& m)
    {
        return m["type"] == "game" && m["event"]["kind"] == "roundComplete";
    }, patience));
}

TEST_CASE("Online, closing during the hold joins at once", "[hold][online][close]")
{
    OnlineGame online(3, 2, longHold);
    online.addHuman(1, false);
    online.addHuman(2, false);
    online.run->start();
    REQUIRE(online.recorder.waitFor(1, ofType("held"), patience));

    const auto started = Clock::now();
    online.run->close();
    online.run.reset();
    CHECK(Clock::now() - started < milliseconds(1000 * test::slowdown));
}
