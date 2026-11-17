#include "core/Service.h"
#include "core/Status.h"

#include "support/Client.h"
#include "support/Games.h"
#include "support/ManualClock.h"
#include "support/Online.h"
#include "support/Recorder.h"

#include <catch2/catch_test_macros.hpp>

#include <functional>
#include <memory>
#include <thread>

using namespace whist;
using namespace std::chrono_literals;
using std::chrono::milliseconds;

namespace
{

bool waitUntil(const std::function<bool()>& done)
{
    const auto deadline = Clock::now() + test::patience;
    while(!done())
    {
        if(Clock::now() > deadline)
            return false;
        std::this_thread::sleep_for(milliseconds(1));
    }
    return true;
}

Json::Value createSolo(const Settings& settings)
{
    Json::Value message = test::typed("createSolo");
    message["settings"] = encodeSettings(settings);
    return message;
}

} // namespace

TEST_CASE("A status line is one line of pairs that starts with a word to search for", "[status]")
{
    ServerStatus status;
    status.connections = 12;
    status.tablesInLobby = 1;
    status.tablesPlaying = 2;
    status.tablesFinished = 3;
    status.gameThreads = 2;
    status.standInMoves = 17;
    status.cpuSeconds = 123.44;
    status.cpuPercent = 7.26;
    status.peakMemoryMb = 45;

    CHECK(statusLine(status) == "status connections=12 tables_lobby=1 tables_playing=2 tables_finished=3 "
                                "game_threads=2 stand_in_moves=17 cpu_percent=7.3 cpu_seconds=123.4 peak_memory_mb=45");
}

TEST_CASE("The CPU meter says how much of a core was used since the last time", "[status]")
{
    const auto clock = std::make_shared<test::ManualClock>();
    ProcessUse use{10.0, 20};
    CpuMeter meter([&] { return use; }, [clock] { return clock->now(); });

    ServerStatus status;
    clock->advance(10s);
    use = {15.0, 31};
    meter.measure(status);
    CHECK(status.cpuSeconds == 15.0);
    CHECK(status.cpuPercent == 50.0);
    CHECK(status.peakMemoryMb == 31);

    // Idle for a while.
    clock->advance(4s);
    meter.measure(status);
    CHECK(status.cpuPercent == 0.0);

    // More than one core.
    clock->advance(2s);
    use.cpuSeconds += 4.0;
    meter.measure(status);
    CHECK(status.cpuPercent == 200.0);

    // No time has passed: no share to give.
    use.cpuSeconds += 1.0;
    meter.measure(status);
    CHECK(status.cpuPercent == 0.0);
}

TEST_CASE("The process's use is read from the system", "[status]")
{
    // Some work first, so that the processor time is not nothing.
    volatile std::uint64_t sum = 0;
    for(std::uint64_t i = 0 ; i < 20'000'000 ; i++)
        sum = sum + i;

    const ProcessUse use = processUse();
    CHECK(use.cpuSeconds > 0.0);
    CHECK(use.peakMemoryMb > 0);
}

TEST_CASE("A service says how many connections, tables and game threads it has", "[status]")
{
    test::Recorder recorder;
    ServiceOptions options;
    options.registry.table.instantFloor = milliseconds(0);
    options.registry.table.threadNiceness = 0;
    const auto service = std::make_shared<Service>(std::move(options));

    ServerStatus none = service->status();
    CHECK(none.connections == 0);
    CHECK(none.tablesInLobby + none.tablesPlaying + none.tablesFinished == 0);
    CHECK(none.gameThreads == 0);
    CHECK(none.standInMoves == 0);

    auto ana = std::make_unique<test::Client>(service, recorder, 1);
    auto bea = std::make_unique<test::Client>(service, recorder, 2);
    auto cora = std::make_unique<test::Client>(service, recorder, 3);

    // A connection counts before it has said hello.
    CHECK(service->status().connections == 3);
    ana->hello();
    bea->hello();
    cora->hello();

    // A game that waits for its human, and a table that waits for its players.
    ana->send(createSolo(test::tableSettings(3, {0}, Level::Cyborg)));
    Json::Value create = test::typed("createTable");
    create["settings"] = encodeSettings(test::onlineSettings(3, 2));
    create["profile"] = test::profile("Bea");
    bea->send(create);

    ServerStatus status = service->status();
    CHECK(status.connections == 3);
    CHECK(status.tablesPlaying == 1);
    CHECK(status.tablesInLobby == 1);
    CHECK(status.tablesFinished == 0);
    CHECK(status.gameThreads == 1);

    // A game of computer players that has ended is a table still, and no longer a thread.
    cora->send(createSolo(test::tableSettings(3, {})));
    REQUIRE(waitUntil([&] { return service->status().tablesFinished == 1; }));
    REQUIRE(waitUntil([&] { return service->status().gameThreads == 1; }));
    CHECK(service->status().tablesPlaying == 1);

    // A connection that closes stops counting once its session is gone.
    bea->drop();
    bea.reset();
    CHECK(service->status().connections == 2);
    ana->drop();
    ana.reset();
    cora->drop();
    cora.reset();
    CHECK(service->status().connections == 0);
}
