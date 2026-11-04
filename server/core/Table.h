#ifndef WHIST_CORE_TABLE_H
#define WHIST_CORE_TABLE_H

#include "core/GameRun.h"
#include "core/Joiner.h"
#include "core/Outbox.h"
#include "core/Protocol.h"
#include "core/Seating.h"
#include "core/Settings.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <vector>

namespace whist
{

struct TableOptions
{
    std::chrono::milliseconds instantFloor{30};
    int threadNiceness = 10;

    // Online: how long the hold after a round waits for the players' Continues at most.
    std::chrono::milliseconds roundHold{30000};

    // Online: how long, from when their connection went, a player's turn is waited for before a
    // computer player takes their seat.
    std::chrono::milliseconds gracePeriod{30000};

    // How long a table is kept once nobody at it is connected.
    std::chrono::milliseconds onlineIdle{10 * 60 * 1000};
    std::chrono::milliseconds soloIdle{30 * 60 * 1000};

    // For tests: the turn timer of every online table that has one, whatever its settings allow.
    std::optional<std::chrono::milliseconds> turnTimerOverride;
};

// The settings, seats, people and current game of one table. Its lock comes after the Registry's
// and before any seat driver's. Messages are sent under it: Outbox::send only queues.
class Table : public std::enable_shared_from_this<Table>
{
public:
    // `seed` draws computer players' names and portraits.
    Table(std::string code, Mode mode, Seating seating, TableOptions options, Outbox& outbox,
          std::shared_ptr<Joiner> joiner, std::uint64_t seed);

    // A game still here goes to the joiner, never joined on this thread.
    ~Table();

    Table(const Table&) = delete;
    Table& operator=(const Table&) = delete;

    const std::string& code() const;
    Mode mode() const;
    TableState state() const;
    unsigned int game() const;
    std::vector<MemberId> members() const;
    bool hasMember(MemberId member) const;
    Settings settings() const;

    // The host starts the first game, or a new one once the last is over. The old game goes to
    // the joiner, and everyone is sent the new tableState. When no thread can be made for the game,
    // the table is left as it was and the answer is Draining.
    std::optional<Refusal> startGame(MemberId by);

    // A bet or a play from an I/O thread. An answer to no open prompt is refused with the code
    // returned, which goes back to the connection it came from; a legal or illegal answer to the
    // open one is the game thread's to handle.
    std::optional<Refusal> answer(MemberId member, const Answer& answer);

    // Solo, while a game is on: holds it at its next rest, or lets it go on.
    std::optional<Refusal> setPaused(MemberId by, bool paused);

    // Solo: only the bot speed and the wait after each round may change once the table exists. A
    // running game takes them up at once. Online: the host's, between games, of which the seats
    // give only their number.
    std::optional<Refusal> updateSettings(MemberId by, const Settings& settings);

    // A Continue for a held round. Solo, the host's ends the hold; online, each seated member's
    // counts. A Continue for any other round, or after the game, is ignored.
    std::optional<Refusal> continueRound(MemberId by, unsigned int round);

    // Sends the member everything they need to pick the game up where it is, and, online, tells the
    // others they are here. A seat a computer player took over is theirs again. False when they are
    // not at this table, and nothing was sent.
    bool sync(MemberId member);

    // A seat a computer player took over while the member was connected is theirs again. Anything
    // else changes nothing.
    void back(MemberId member);

    // Called every second or so. Replaces members who are away past the grace period, hands over a
    // host who is, and lets go of those who left a finished game. True when the table has been idle
    // too long, and is to close.
    bool reap(Clock::time_point now);

    // The game's callback, from its own thread: something it waits for or has done is for the table
    // to tell everyone. Calls for any game but the current one are ignored.
    void runChanged(unsigned int game);

    TableView viewFor(MemberId member) const;

    // Ends the game, if one is running, and tells everyone the table is closed. That is the last
    // message anyone gets from this table.
    void close();

    // For tests: the outcome of the current game, if there is one, and how many rounds it has scored.
    std::optional<RunOutcome> outcome() const;
    std::size_t scoredRounds() const;

    // ---- Online, between games: in the lobby, or once a game is over (TableLobby.cpp) ----

    // A newcomer takes the first open seat, and is sent a sync; the others a tableState. The profile
    // is clean and checked.
    std::optional<Refusal> join(MemberId member, Profile profile);

    struct Left
    {
        std::optional<Refusal> refused;

        // Nobody is left, so the table is to close.
        bool empty = false;
    };

    // Between games the member's seat opens. During a game it stays theirs, with a computer player in
    // it, and they are sent nothing more of it until they join again. A host hands over either way.
    Left leave(MemberId member);

    std::optional<Refusal> takeSeat(MemberId member, unsigned int seat);

    // `language` is the host's, for the names of computer players.
    std::optional<Refusal> setSeat(MemberId by, unsigned int seat, SeatKind kind, std::optional<Level> level,
                                   std::string_view language);

    std::optional<Refusal> updateProfile(MemberId member, Profile profile);

    // The member's connection has gone. Online, everyone is told and their turn is waited for through
    // the grace period. A game nobody is connected to holds.
    void presenceChanged(MemberId member);

private:
    TableState stateLocked() const;
    TableView viewLocked(MemberId member) const;
    void broadcastLocked(std::optional<MemberId> except = std::nullopt) const;
    void syncLocked(MemberId member) const;

    // Solo, by the host, with a game playing.
    std::optional<Refusal> checkSoloPlayingLocked(MemberId by) const;

    // Online, by a member, with no game on.
    std::optional<Refusal> checkBetweenGamesLocked(MemberId by) const;

    // A game that is over lets go of the members who left it: their seats open. True when any did.
    bool settleLocked();

    // The member's connection is here: they are not away, and have the seat back that a computer
    // player took over.
    void arriveLocked(MemberId member);

    // Works out whether anybody is connected, for the idle timer and for a game that holds for nobody.
    void refreshPresenceLocked();

    const std::string code_;
    const Mode mode_;
    const TableOptions options_;
    Outbox& outbox_;
    const std::shared_ptr<Joiner> joiner_;

    // Shared with each game, which can outlive the table on the joiner's queue. Ids are never
    // reused at a table, so an answer from an earlier game cannot match a new prompt.
    const std::shared_ptr<std::atomic<std::uint64_t>> promptIds_ = std::make_shared<std::atomic<std::uint64_t>>(0);

    mutable std::mutex mutex_;
    Seating seating_;
    std::mt19937_64 random_;
    TableState state_ = TableState::Lobby;
    bool paused_ = false;
    unsigned int game_ = 0;
    std::unique_ptr<GameRun> run_;

    // Since when nobody at the table has been connected.
    std::optional<Clock::time_point> idleSince_;
};

} // namespace whist

#endif
