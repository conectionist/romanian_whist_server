#ifndef WHIST_CORE_GAMERUN_H
#define WHIST_CORE_GAMERUN_H

#include "core/Outbox.h"
#include "core/Protocol.h"
#include "core/Publisher.h"
#include "core/SeatDriver.h"
#include "core/Settings.h"

#include <romanian_whist/GameEngine.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <thread>
#include <vector>

namespace whist
{

struct GameRunSetup
{
    unsigned int game = 1;

    // Checked by checkSettings(): every seat filled.
    Settings settings;

    // Per seat: the member who plays it. Every human seat has one; computer seats have none.
    std::vector<std::optional<MemberId>> seatMembers;

    // People who see the game without a seat.
    std::vector<MemberId> watchers;

    std::optional<std::uint32_t> shuffleSeed;

    std::chrono::milliseconds instantFloor{30};

    // Online only: the hold after a round waits for the humans connected then, at most this long.
    // None in solo, whose hold waits for as long as it takes.
    std::optional<std::chrono::milliseconds> roundHold;

    // Game threads run below the I/O threads, so a table of Reckoners never slows the server.
    int threadNiceness = 10;

    std::shared_ptr<std::atomic<std::uint64_t>> promptIds;

    // Called on the game thread, with no lock held, when a seat that waits for a member who is away
    // changes, and once the game is over: the table has something new to tell everyone.
    std::function<void()> changed;

    // Registered before everything else, so they see each event first. For tests and tools.
    std::vector<romanian_whist::IGameObserver*> firstObservers;
};

enum class RunOutcome
{
    Running,
    Finished,
    Stopped,
    Abandoned,
    Failed
};

// One game: the engine, its seats, its publisher and the thread it runs on. Built and started on
// an I/O thread, played on its own thread, and destroyed on the joiner's.
class GameRun
{
public:
    GameRun(GameRunSetup setup, Outbox& outbox);

    // Closes the game and joins its thread. Never destroy one on an I/O thread: hand it to the
    // Joiner.
    ~GameRun();

    GameRun(const GameRun&) = delete;
    GameRun& operator=(const GameRun&) = delete;

    // Starts the thread. Called once, after everything that can fail.
    void start();

    // Any thread, any number of times. Ends the game at its next boundary, or at once for a seat
    // waiting on its human. Does not wait for the thread.
    void close();

    // Any thread. Closes the game, and nothing more is sent from it after this returns, not even its
    // gameStopped. For a table letting go of a game, so its messages never follow the table's own.
    void silence();

    // True once the game thread is done with the engine.
    bool finished() const;

    // True once the game has sent gameOver or its thread is done: nothing more of it will follow.
    bool over() const;
    RunOutcome outcome() const;
    unsigned int game() const;

    // Any thread. The member's answer, to the seat they play in this game, whatever seat the table
    // gives them now.
    SeatDriver::Submitted submit(MemberId member, const Answer& answer);

    // Any thread. Pause or resume, or change the speed and the wait.
    void setPaused(bool paused);
    void setPacing(Pacing pacing, bool waitAfterRound);

    // Any thread. The member's Continue for a held round, from the seat they play in this game.
    void continueRound(MemberId member, unsigned int round);

    // Any thread. Online: the member has gone, so the hold no longer waits for them.
    void excuse(MemberId member);

    // Any thread. The member's connection is gone, and their turn is waited for until `graceEnds` at
    // most, or back again. Of the seat they play in this game.
    void away(MemberId member, Clock::time_point graceEnds);
    void here(MemberId member);

    // Any thread. The seat's computer player plays from now on. A member who `silent`ly leaves is
    // sent nothing of the game, and the round hold stops waiting for them.
    void replace(MemberId member, bool silent);

    // Any thread. The member has their seat again, and is sent the game again. False when it was
    // theirs already.
    bool reclaim(MemberId member);

    bool replaced(unsigned int seat) const;

    // The seat whose member is away, and the time left until the table stops waiting for them.
    std::optional<WaitingFor> waitingFor() const;

    // Any thread. Whether nobody who sees the game is connected: it holds at its next rest.
    void setNobodyHere(bool nobodyHere);

    // Any thread. The member's sync, with the open prompt of the seat they play in this game.
    void sendSync(MemberId member, TableView table);

    // Whether the member sees this game, and from which seat; a seat of none is a watcher's.
    std::optional<Viewer> viewer(MemberId member) const;

    const Publisher& publisher() const;

private:
    void play(romanian_whist::GameSetup setup);

    const unsigned int game_;
    const int threadNiceness_;
    const std::function<void()> changed_;
    const std::vector<romanian_whist::IGameObserver*> firstObservers_;
    const std::vector<Viewer> viewers_;
    std::optional<romanian_whist::GameSetup> engineSetup_;

    // The order is the design: destruction runs in reverse, so the thread is joined first, and the
    // engine goes before the observers it points at.
    std::vector<std::unique_ptr<SeatDriver>> drivers_;
    Publisher publisher_;
    romanian_whist::GameEngine engine_;
    std::atomic<RunOutcome> outcome_{RunOutcome::Running};
    std::jthread thread_;
};

} // namespace whist

#endif
