#ifndef WHIST_CORE_TABLE_H
#define WHIST_CORE_TABLE_H

#include "core/GameRun.h"
#include "core/Joiner.h"
#include "core/Outbox.h"
#include "core/Protocol.h"
#include "core/Settings.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace whist
{

// A person at a table: in a seat, or watching.
struct TableMember
{
    MemberId id = 0;
    std::optional<unsigned int> seat;
};

struct TableOptions
{
    std::chrono::milliseconds instantFloor{30};
    int threadNiceness = 10;
};

// The settings, seats, people and current game of one table. Its lock comes after the Registry's
// and before any seat driver's. Messages are sent under it: Outbox::send only queues.
class Table
{
public:
    Table(std::string code, Mode mode, Settings settings, std::vector<TableMember> members, MemberId host,
          TableOptions options, Outbox& outbox, std::shared_ptr<Joiner> joiner);

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

    // The host starts the first game, or a new one once the last is over. The old game goes to
    // the joiner, and everyone is sent the new tableState.
    std::optional<ErrorCode> startGame(MemberId by);

    // A bet or a play from an I/O thread. An answer to no open prompt is told so; a legal or
    // illegal answer to the open one is the game thread's to handle.
    void answer(MemberId member, const Answer& answer);

    // Sends the member everything they need to pick the game up where it is.
    void sync(MemberId member);

    TableView viewFor(MemberId member) const;

    // Ends the game, if one is running, and tells everyone the table is closed.
    void close();

    // For tests: the outcome of the current game, if there is one.
    std::optional<RunOutcome> outcome() const;

private:
    TableState stateLocked() const;
    TableView viewLocked(MemberId member) const;
    std::optional<unsigned int> seatOfLocked(MemberId member) const;
    void broadcastLocked() const;

    const std::string code_;
    const Mode mode_;
    const TableOptions options_;
    Outbox& outbox_;
    const std::shared_ptr<Joiner> joiner_;

    // Shared with each game, which can outlive the table on the joiner's queue. Ids are never
    // reused at a table, so an answer from an earlier game cannot match a new prompt.
    const std::shared_ptr<std::atomic<std::uint64_t>> promptIds_ = std::make_shared<std::atomic<std::uint64_t>>(0);

    mutable std::mutex mutex_;
    Settings settings_;
    std::vector<TableMember> members_;
    MemberId host_;
    TableState state_ = TableState::Lobby;
    unsigned int game_ = 0;
    std::unique_ptr<GameRun> run_;
};

} // namespace whist

#endif
