#include "core/Limits.h"
#include "core/RunningGames.h"

#include "support/ManualClock.h"

#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <optional>
#include <vector>

using namespace whist;
using namespace std::chrono_literals;

namespace
{

struct Fixture
{
    std::shared_ptr<test::ManualClock> clock = std::make_shared<test::ManualClock>();

    LimitOptions options() const
    {
        return clock->limits();
    }
};

} // namespace

TEST_CASE("A token bucket lets a burst through, then the rate", "[limits][bucket]")
{
    const Fixture fixture;
    TokenBucket bucket(20, 40, fixture.clock->now());

    for(int i = 0 ; i < 40 ; i++)
        CHECK(bucket.take(fixture.clock->now()));
    CHECK_FALSE(bucket.take(fixture.clock->now()));
    CHECK_FALSE(bucket.take(fixture.clock->now()));

    // Twenty a second is one every 50 ms.
    fixture.clock->advance(49ms);
    CHECK_FALSE(bucket.take(fixture.clock->now()));
    fixture.clock->advance(1ms);
    CHECK(bucket.take(fixture.clock->now()));
    CHECK_FALSE(bucket.take(fixture.clock->now()));

    fixture.clock->advance(500ms);
    for(int i = 0 ; i < 10 ; i++)
        CHECK(bucket.take(fixture.clock->now()));
    CHECK_FALSE(bucket.take(fixture.clock->now()));
}

TEST_CASE("A token bucket never holds more than its burst", "[limits][bucket]")
{
    const Fixture fixture;
    TokenBucket bucket(20, 40, fixture.clock->now());
    for(int i = 0 ; i < 40 ; i++)
        REQUIRE(bucket.take(fixture.clock->now()));

    fixture.clock->advance(std::chrono::hours(1));
    for(int i = 0 ; i < 40 ; i++)
        CHECK(bucket.take(fixture.clock->now()));
    CHECK_FALSE(bucket.take(fixture.clock->now()));
}

TEST_CASE("A token bucket with no rate never refuses, and a clock that steps back adds nothing", "[limits][bucket]")
{
    const Fixture fixture;
    TokenBucket unlimited(0, 0, fixture.clock->now());
    for(int i = 0 ; i < 1000 ; i++)
        CHECK(unlimited.take(fixture.clock->now()));

    const Clock::time_point start = fixture.clock->now();
    TokenBucket bucket(1, 1, start);
    REQUIRE(bucket.take(start));
    CHECK_FALSE(bucket.take(start - 10s));
    CHECK_FALSE(bucket.take(start));
    CHECK(bucket.take(start + 1s));
}

TEST_CASE("Connections from one address are counted, and given back", "[limits]")
{
    const Fixture fixture;
    LimitOptions options = fixture.options();
    options.connectionsPerAddress = 3;
    Limits limits(options);

    for(int i = 0 ; i < 3 ; i++)
        CHECK(limits.admitConnection("203.0.113.9"));
    CHECK_FALSE(limits.admitConnection("203.0.113.9"));
    CHECK_FALSE(limits.admitConnection("203.0.113.9"));

    // Another address has its own.
    CHECK(limits.admitConnection("203.0.113.10"));

    limits.releaseConnection("203.0.113.9");
    CHECK(limits.admitConnection("203.0.113.9"));
    CHECK_FALSE(limits.admitConnection("203.0.113.9"));

    // Everything given back, the address is forgotten, and giving back more does no harm.
    for(int i = 0 ; i < 5 ; i++)
    {
        limits.releaseConnection("203.0.113.9");
        limits.releaseConnection("203.0.113.10");
    }
    CHECK(limits.remembered() == 0);
    CHECK(limits.admitConnection("203.0.113.9"));
}

TEST_CASE("An empty address, and a limit of 0, are not limited", "[limits]")
{
    const Fixture fixture;
    LimitOptions options = fixture.options();
    options.connectionsPerAddress = 1;
    options.tablesPerAddressPerHour = 1;
    options.failedJoinsPerAddress = 1;
    Limits limited(options);

    for(int i = 0 ; i < 5 ; i++)
    {
        CHECK(limited.admitConnection(""));
        CHECK(limited.takeTable(""));
        limited.noteFailedJoin("");
        CHECK_FALSE(limited.joinsBlocked(""));
    }
    CHECK(limited.remembered() == 0);

    Limits unlimited(fixture.options());
    for(int i = 0 ; i < 100 ; i++)
    {
        CHECK(unlimited.admitConnection("203.0.113.9"));
        CHECK(unlimited.takeTable("203.0.113.9"));
        unlimited.noteFailedJoin("203.0.113.9");
        CHECK_FALSE(unlimited.joinsBlocked("203.0.113.9"));
    }
    CHECK(unlimited.remembered() == 0);
}

TEST_CASE("Tables made from one address are counted over an hour", "[limits]")
{
    const Fixture fixture;
    LimitOptions options = fixture.options();
    options.tablesPerAddressPerHour = 3;
    Limits limits(options);

    CHECK(limits.takeTable("203.0.113.9"));
    fixture.clock->advance(10min);
    CHECK(limits.takeTable("203.0.113.9"));
    fixture.clock->advance(10min);
    CHECK(limits.takeTable("203.0.113.9"));
    CHECK_FALSE(limits.takeTable("203.0.113.9"));
    CHECK(limits.takeTable("203.0.113.10"));

    // The first table's hour ends at 60 minutes, the second's at 70.
    fixture.clock->advance(39min + 59s);
    CHECK_FALSE(limits.takeTable("203.0.113.9"));
    fixture.clock->advance(1s);
    CHECK(limits.takeTable("203.0.113.9"));
    CHECK_FALSE(limits.takeTable("203.0.113.9"));

    fixture.clock->advance(10min);
    CHECK(limits.takeTable("203.0.113.9"));
}

TEST_CASE("A table that was refused after all is given back", "[limits]")
{
    const Fixture fixture;
    LimitOptions options = fixture.options();
    options.tablesPerAddressPerHour = 2;
    Limits limits(options);

    CHECK(limits.takeTable("203.0.113.9"));
    CHECK(limits.takeTable("203.0.113.9"));
    limits.refundTable("203.0.113.9");
    CHECK(limits.takeTable("203.0.113.9"));
    CHECK_FALSE(limits.takeTable("203.0.113.9"));

    // Nothing to give back, nothing happens.
    limits.refundTable("203.0.113.77");
    limits.refundTable("");
    CHECK(limits.takeTable("203.0.113.77"));
}

TEST_CASE("An address that failed to join too often is blocked until its failures are old", "[limits]")
{
    const Fixture fixture;
    LimitOptions options = fixture.options();
    options.failedJoinsPerAddress = 3;
    options.failedJoinWindow = 10min;
    Limits limits(options);

    CHECK_FALSE(limits.joinsBlocked("203.0.113.9"));
    limits.noteFailedJoin("203.0.113.9");
    fixture.clock->advance(3min);
    limits.noteFailedJoin("203.0.113.9");
    CHECK_FALSE(limits.joinsBlocked("203.0.113.9"));
    fixture.clock->advance(3min);
    limits.noteFailedJoin("203.0.113.9");
    CHECK(limits.joinsBlocked("203.0.113.9"));
    CHECK_FALSE(limits.joinsBlocked("203.0.113.10"));

    // The window slides: the first failure is 10 minutes old at 10 minutes.
    fixture.clock->advance(3min + 59s);
    CHECK(limits.joinsBlocked("203.0.113.9"));
    fixture.clock->advance(1s);
    CHECK_FALSE(limits.joinsBlocked("203.0.113.9"));

    // Two failures left, so one more blocks again.
    limits.noteFailedJoin("203.0.113.9");
    CHECK(limits.joinsBlocked("203.0.113.9"));
}

TEST_CASE("Failures that race past the check do not pile up", "[limits]")
{
    const Fixture fixture;
    LimitOptions options = fixture.options();
    options.failedJoinsPerAddress = 2;
    Limits limits(options);

    for(int i = 0 ; i < 50 ; i++)
        limits.noteFailedJoin("203.0.113.9");
    CHECK(limits.joinsBlocked("203.0.113.9"));

    // Only two were kept, so they are the only ones that have to age out.
    fixture.clock->advance(10min);
    CHECK_FALSE(limits.joinsBlocked("203.0.113.9"));
}

TEST_CASE("Addresses that never come back are forgotten", "[limits]")
{
    const Fixture fixture;
    LimitOptions options = fixture.options();
    options.tablesPerAddressPerHour = 5;
    options.failedJoinsPerAddress = 5;
    Limits limits(options);

    for(int i = 0 ; i < 200 ; i++)
    {
        const std::string address = "198.51.100." + std::to_string(i);
        limits.takeTable(address);
        limits.noteFailedJoin(address);
    }
    CHECK(limits.remembered() == 400);

    // A busy server sweeps as it goes, so the first call after a long quiet time clears them.
    fixture.clock->advance(2h);
    limits.takeTable("203.0.113.9");
    CHECK(limits.remembered() == 1);
}

TEST_CASE("Running games are counted, and the most that may run is held to", "[limits][games]")
{
    const auto games = std::make_shared<RunningGames>(2);

    std::optional<RunningGames::Slot> first = games->take();
    std::optional<RunningGames::Slot> second = games->take();
    REQUIRE(first);
    REQUIRE(second);
    CHECK(games->count() == 2);
    CHECK_FALSE(games->take());
    CHECK(games->count() == 2);

    first->release();
    CHECK(games->count() == 1);

    // Safe to repeat, and destroying a released slot gives nothing back twice.
    first->release();
    first.reset();
    CHECK(games->count() == 1);

    std::optional<RunningGames::Slot> third = games->take();
    REQUIRE(third);
    CHECK_FALSE(games->take());

    // A moved slot is still one place.
    std::optional<RunningGames::Slot> moved = std::move(second);
    second.reset();
    CHECK(games->count() == 2);
    moved.reset();
    CHECK(games->count() == 1);
    third.reset();
    CHECK(games->count() == 0);
}

TEST_CASE("Instant games have a most of their own, inside the most of all games", "[limits][games][instant]")
{
    const auto games = std::make_shared<RunningGames>(3, 1);

    std::optional<RunningGames::Slot> instant = games->take(true);
    REQUIRE(instant);
    CHECK_FALSE(games->take(true));
    CHECK(games->attendedInstant() == 1);

    // Games at other speeds take the places left, and the refusal says which most it was.
    std::optional<RunningGames::Slot> second = games->take();
    std::optional<RunningGames::Slot> third = games->take();
    REQUIRE(second);
    REQUIRE(third);
    RunningGames::Full full = RunningGames::Full::Instant;
    CHECK_FALSE(games->take(false, &full));
    CHECK(full == RunningGames::Full::All);
    third.reset();
    CHECK_FALSE(games->take(true, &full));
    CHECK(full == RunningGames::Full::Instant);
    CHECK(games->count() == 2);

    // A refused Instant game took nothing, and the Instant place comes back with its game.
    instant.reset();
    CHECK(games->attendedInstant() == 0);
    CHECK(games->attended() == 1);
    CHECK(games->take(true));
}

TEST_CASE("An Instant game is refused by the most of all games, with Instant places free", "[limits][games][instant]")
{
    const auto games = std::make_shared<RunningGames>(2, 2);

    std::optional<RunningGames::Slot> first = games->take();
    std::optional<RunningGames::Slot> second = games->take();
    REQUIRE(first);
    REQUIRE(second);
    RunningGames::Full full = RunningGames::Full::Instant;
    CHECK_FALSE(games->take(true, &full));
    CHECK(full == RunningGames::Full::All);

    const RunningGames::Counts counts = games->counts();
    CHECK(counts.threads == 2);
    CHECK(counts.attended == 2);
    CHECK(counts.attendedInstant == 0);
}

TEST_CASE("A game becomes Instant only while there is room, and an Instant game nobody is at does not count", "[limits][games][instant]")
{
    const auto games = std::make_shared<RunningGames>(0, 1);

    std::optional<RunningGames::Slot> ana = games->take(true);
    std::optional<RunningGames::Slot> bea = games->take();
    REQUIRE(ana);
    REQUIRE(bea);
    CHECK_FALSE(bea->setInstant(true));
    CHECK(games->attendedInstant() == 1);

    ana->setAttended(false);
    CHECK(games->attendedInstant() == 0);
    CHECK(bea->setInstant(true));
    CHECK(games->attendedInstant() == 1);

    // Coming back is never refused, so the most is soft for returns, as it is for all games.
    ana->setAttended(true);
    CHECK(games->attendedInstant() == 2);
    CHECK(ana->setInstant(false));
    CHECK(games->attendedInstant() == 1);

    // A game nobody is at may become Instant, and counts when someone is back.
    ana->setAttended(false);
    CHECK(ana->setInstant(true));
    CHECK(games->attendedInstant() == 1);
    ana->setAttended(true);
    CHECK(games->attendedInstant() == 2);

    ana.reset();
    bea.reset();
    CHECK(games->attendedInstant() == 0);
    CHECK(games->attended() == 0);
    CHECK(games->count() == 0);
}

TEST_CASE("A maximum of 0 allows any number of games", "[limits][games]")
{
    const auto games = std::make_shared<RunningGames>(0);

    std::vector<RunningGames::Slot> slots;
    for(int i = 0 ; i < 500 ; i++)
    {
        std::optional<RunningGames::Slot> slot = games->take();
        REQUIRE(slot);
        slots.push_back(std::move(*slot));
    }
    CHECK(games->count() == 500);
    slots.clear();
    CHECK(games->count() == 0);
}

TEST_CASE("A slot outlives the counter it came from", "[limits][games]")
{
    std::optional<RunningGames::Slot> slot;
    {
        const auto games = std::make_shared<RunningGames>(1);
        slot = games->take();
    }
    CHECK(slot);
    slot.reset();
}
