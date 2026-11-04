#include "core/Codec.h"
#include "core/GameRun.h"

#include "support/Games.h"
#include "support/Oracle.h"
#include "support/Recorder.h"
#include "support/ScriptedHuman.h"

#include <catch2/catch_test_macros.hpp>

#include <memory>

using namespace whist;
using std::chrono::milliseconds;

namespace
{

const milliseconds patience(60000 * test::slowdown);
constexpr MemberId watcher = 99;

struct LeakGame
{
    unsigned int seats;
    std::vector<unsigned int> humans;
    romanian_whist::GameStructure structure;
    bool allOneCardForehead;
    std::uint32_t seed;
};

// Plays one game with scripted humans at `humans` and a watcher, and returns the whole log. Every
// so often a human reloads while the game waits for them. Every other game waits after each round,
// so the syncs taken then carry the hold.
std::vector<test::Entry> play(const LeakGame& spec)
{
    test::Recorder recorder;
    Outbox outbox;

    Settings settings = test::tableSettings(spec.seats, spec.humans, Level::Random);
    settings.structure = spec.structure;
    settings.allOneCardForehead = spec.allOneCardForehead;
    settings.waitAfterRound = spec.seed % 2 == 0;

    std::vector<std::optional<MemberId>> seatMembers(spec.seats);
    std::vector<Viewer> viewers;
    for(unsigned int seat : spec.humans)
    {
        seatMembers[seat] = seat + 1;
        viewers.push_back({seat + 1, seat});
    }
    viewers.push_back({watcher, std::nullopt});

    test::Oracle oracle(recorder, viewers);
    GameRunSetup setup = test::quickGame(settings, seatMembers, spec.seed);
    setup.watchers = {watcher};
    setup.firstObservers = {&oracle};

    for(const Viewer& viewer : viewers)
        outbox.attach(viewer.member, recorder.connect(viewer.member));

    auto run = std::make_unique<GameRun>(std::move(setup), outbox);

    std::vector<std::unique_ptr<test::ScriptedHuman>> humans;
    for(unsigned int seat : spec.humans)
    {
        const MemberId member = seat + 1;
        auto prompts = std::make_shared<std::atomic<int>>(0);
        humans.push_back(std::make_unique<test::ScriptedHuman>(
            recorder, member, [&run, member](const Answer& answer) { run->submit(member, answer); }, test::Behaviour::Good,
            [&run, member, prompts](const Json::Value&)
            {
                if(++*prompts % 17 == 0)
                {
                    run->sendSync(member, TableView{});
                    run->sendSync(watcher, TableView{});
                }
                return false;
            },
            [&run, member](unsigned int round)
            {
                run->sendSync(member, TableView{});
                run->continueRound(round);
            }));
    }

    run->start();
    for(const auto& human : humans)
    {
        REQUIRE(human->waitUntilOver(patience));
        CHECK(human->problems().empty());
    }
    const auto deadline = Clock::now() + patience;
    while(!run->finished() && Clock::now() < deadline)
        std::this_thread::sleep_for(milliseconds(1));
    CHECK(run->outcome() == RunOutcome::Finished);
    humans.clear();
    run.reset();
    return recorder.entries();
}

} // namespace

TEST_CASE("No message carries a card its member may not see", "[leak]")
{
    using romanian_whist::GameStructure;

    std::vector<LeakGame> games;
    std::uint32_t seed = 100;
    for(unsigned int seats = 2 ; seats <= 6 ; seats++)
    {
        for(GameStructure structure : {GameStructure::S_181, GameStructure::S_818})
        {
            for(bool allForehead : {false, true})
            {
                // Humans at different seats each time, and once at every seat.
                std::vector<unsigned int> humans{seed % seats};
                if(seats > 2)
                    humans.push_back((seed / 3 + 1) % seats == humans[0] ? (humans[0] + 1) % seats : (seed / 3 + 1) % seats);
                if(allForehead && structure == GameStructure::S_818)
                {
                    humans.clear();
                    for(unsigned int seat = 0 ; seat < seats ; seat++)
                        humans.push_back(seat);
                }
                games.push_back({seats, humans, structure, allForehead, seed++});
            }
        }
    }

    std::size_t messages = 0;
    for(const LeakGame& game : games)
    {
        INFO(game.seats << " seats, seed " << game.seed);
        const std::vector<test::Entry> entries = play(game);
        messages += entries.size();
        const std::vector<std::string> leaks = test::scanForLeaks(entries);
        CHECK(leaks.empty());
        for(std::size_t i = 0 ; i < std::min<std::size_t>(leaks.size(), 5) ; i++)
            UNSCOPED_INFO(leaks[i]);
    }
    CHECK(messages > 10000);
}

TEST_CASE("The leak scan finds a card where it should not be", "[leak]")
{
    // A seat's own Forehead card, sent to that seat: the case the engine's README warns about.
    test::Recorder recorder;
    test::Mark round;
    round.round = 0;
    round.visible[1] = {"AS"};
    round.visible[2] = {"KH"};
    recorder.mark(round);

    Outbox outbox;
    outbox.attach(1, recorder.connect(1));
    Json::Value message = errorMessage(ErrorCode::Illegal, "KH");
    outbox.send(1, message);
    outbox.send(1, errorMessage(ErrorCode::Illegal, "AS"));

    test::Mark played;
    played.card = "KH";
    recorder.mark(played);
    outbox.send(1, errorMessage(ErrorCode::Illegal, "KH"));

    // The engine's own spelling of the ten is caught too.
    outbox.send(1, errorMessage(ErrorCode::Illegal, "10D"));

    const auto leaks = test::scanForLeaks(recorder.entries());
    REQUIRE(leaks.size() == 2);
    CHECK(leaks[0].find("KH") != std::string::npos);
    CHECK(leaks[1].find("10D") != std::string::npos);
}
