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
};

// The settings, seats, people and current game of one table. Its lock comes after the Registry's
// and before any seat driver's. Messages are sent under it: Outbox::send only queues.
class Table
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
    // others they are here. False when they are not at this table, and nothing was sent.
    bool sync(MemberId member);

    TableView viewFor(MemberId member) const;

    // Ends the game, if one is running, and tells everyone the table is closed. That is the last
    // message anyone gets from this table.
    void close();

    // For tests: the outcome of the current game, if there is one.
    std::optional<RunOutcome> outcome() const;

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

    // The member's seat opens, and a host hands over to whoever has been there longest. Not while
    // a game is on.
    Left leave(MemberId member);

    std::optional<Refusal> takeSeat(MemberId member, unsigned int seat);

    // `language` is the host's, for the names of computer players.
    std::optional<Refusal> setSeat(MemberId by, unsigned int seat, SeatKind kind, std::optional<Level> level,
                                   std::string_view language);

    std::optional<Refusal> updateProfile(MemberId member, Profile profile);

    // Online: the member's connection has gone. Everyone is told, and a hold after a round stops
    // waiting for them.
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
};

} // namespace whist

#endif
