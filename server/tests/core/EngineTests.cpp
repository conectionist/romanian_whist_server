#include "core/BuildInfo.h"

#include <catch2/catch_test_macros.hpp>

#include <romanian_whist/AiMoveProvider.h>
#include <romanian_whist/GameEngine.h>
#include <romanian_whist/IGameObserver.h>
#include <romanian_whist/Snapshot.h>
#include <romanian_whist/strategies/RandomCardStrategy.h>

#include <memory>
#include <optional>
#include <string>

using namespace romanian_whist;

namespace
{

struct FirstRound : IGameObserver
{
    std::optional<GameSnapshot> spectator;
    std::optional<GameSnapshot> seat0;

    void onRoundStarted(const GameEngine& engine) override
    {
        if(spectator)
            return;

        spectator = snapshotFrom(engine, std::nullopt);
        seat0 = snapshotFrom(engine, Seat{0});
    }
};

} // namespace

TEST_CASE("The engine submodule is the one with viewer-filtered snapshots", "[engine]")
{
    CHECK(whist::engineVersion().starts_with("5."));

    GameSetup setup;
    for(unsigned int seat = 0 ; seat < 3 ; seat++)
        setup.seats.push_back({"Seat " + std::to_string(seat),
                               std::make_unique<AiMoveProvider>(std::make_unique<RandomCardStrategy>(seat))});
    setup.shuffleSeed = 7;

    FirstRound firstRound;
    GameEngine engine;
    engine.addObserver(&firstRound);
    engine.start(std::move(setup));
    engine.run();

    REQUIRE(firstRound.spectator);
    REQUIRE(firstRound.seat0);
    REQUIRE(firstRound.seat0->roundType == RoundType::Normal);

    for(const SeatSnapshot& seat : firstRound.spectator->seats)
        CHECK_FALSE(seat.hand);

    REQUIRE(firstRound.seat0->seats.size() == 3);
    CHECK(firstRound.seat0->seats[0].hand);
    CHECK_FALSE(firstRound.seat0->seats[1].hand);
    CHECK_FALSE(firstRound.seat0->seats[2].hand);
}
