#ifndef WHIST_CORE_REGISTRY_H
#define WHIST_CORE_REGISTRY_H

#include "core/Joiner.h"
#include "core/Outbox.h"
#include "core/Protocol.h"
#include "core/Settings.h"
#include "core/Table.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

namespace whist
{

struct RegistryOptions
{
    TableOptions table;

    // 64 random bits a call. Tokens and table codes come from it, so tests can make them fixed.
    std::function<std::uint64_t()> random;

    // A member nobody has been connected as, and who is at no table, is forgotten after this long.
    std::chrono::milliseconds forgetMember{60 * 60 * 1000};

    // The most games someone is at that may run at once. 0: any number.
    std::size_t maxRunningGames = 0;

    // Of those, the most at Instant speed. 0: any number.
    std::size_t maxInstantGames = 0;

    // Of the Instant games, the most that one address may have started. 0: any number.
    std::size_t maxInstantGamesPerAddress = 0;

    // The most tables that may exist at once, whoever is at them. 0: any number.
    std::size_t maxTables = 0;
};

// Every table and every member the server knows. Its lock comes first: the Registry may take a
// table's lock, never the other way round.
class Registry
{
public:
    explicit Registry(Outbox& outbox, RegistryOptions options = {});

    // Shuts down first.
    ~Registry();

    Registry(const Registry&) = delete;
    Registry& operator=(const Registry&) = delete;

    struct NewMember
    {
        MemberId id = 0;
        std::string token;
    };

    // A new person, with a token of 128 random bits. Attach their connection to the Outbox before
    // they create or join a table, so they miss none of its first messages.
    NewMember newMember();
    std::optional<MemberId> findMember(std::string_view token) const;

    struct Created
    {
        std::shared_ptr<Table> table;
        std::optional<ErrorCode> error;
        std::string detail;
    };

    // A private table with the member at its one human seat, or watching when there is none. The
    // game starts at once, and counts as one of `address`'s.
    Created createSolo(MemberId member, Settings settings, const std::string& address = {});

    // An online table in its lobby, with the member as its host in the first open seat.
    Created createTable(MemberId member, Settings settings, Profile profile);

    // The member takes the first open seat at the online table with that code.
    std::optional<Refusal> joinTable(MemberId member, std::string_view code, Profile profile);

    // Solo, the table closes. Online, between games, the member's seat opens, and the last one out
    // closes the table.
    std::optional<Refusal> leaveTable(MemberId member);

    std::shared_ptr<Table> findTable(std::string_view code) const;
    std::shared_ptr<Table> tableOf(MemberId member) const;
    std::size_t tableCount() const;

    // How many game threads are running now, a game nobody is at included.
    std::size_t runningGames() const;

    // How many of them count against the most that may run: the games someone is at, or was within
    // the grace period as of the last reap, and none that has ended.
    std::size_t countedGames() const;

    // Of the counted games, those at Instant speed.
    std::size_t instantGames() const;

    // The three counts above, read at one moment.
    RunningGames::Counts gameCounts() const;

    // How many of the running games are attended, as Table::attended says: the ones a drain waits
    // for, since a game nobody is at cannot go on.
    std::size_t attendedGames(Clock::time_point now) const;

    // How many moves computer players have made for members, since the Registry was made.
    std::uint64_t standInMoves() const;

    struct TableCounts
    {
        std::size_t lobby = 0;
        std::size_t playing = 0;
        std::size_t finished = 0;
    };

    // The tables there are, by what they are doing.
    TableCounts tableCounts() const;

    void closeTable(std::string_view code);

    // Called every second or so. Closes tables that have been idle too long, and forgets members who
    // are at no table and have not been connected for long.
    void reap(Clock::time_point now);

    // From now on no table is made and no game starts; everything else goes on. Returns false when
    // it had begun already.
    bool beginDrain();
    bool draining() const;

    // Refuses new tables, closes every table, and joins every game thread. Call it before the
    // Outbox's connections are gone.
    void shutdown();

    const Joiner& joiner() const;

private:
    struct MemberRecord
    {
        std::string token;
        std::string table;

        // Since when the member has been at no table and not connected.
        std::optional<Clock::time_point> unseenSince{};
    };

    std::string newToken();
    std::string newCode();

    // Takes the table off the books; the caller closes it, without this lock.
    std::shared_ptr<Table> removeLocked(std::string_view code);

    Outbox& outbox_;
    const std::shared_ptr<RunningGames> runningGames_;
    const std::shared_ptr<std::atomic<std::uint64_t>> standInMoves_ = std::make_shared<std::atomic<std::uint64_t>>(0);
    const std::shared_ptr<std::atomic<bool>> draining_ = std::make_shared<std::atomic<bool>>(false);
    const TableOptions tableOptions_;
    const std::size_t maxTables_;
    const std::chrono::milliseconds forgetMember_;
    const std::shared_ptr<Joiner> joiner_ = std::make_shared<Joiner>();

    mutable std::mutex mutex_;
    std::function<std::uint64_t()> random_;
    bool closing_ = false;
    MemberId nextMember_ = 1;
    std::unordered_map<MemberId, MemberRecord> members_;
    std::unordered_map<std::string, MemberId> tokens_;
    std::map<std::string, std::shared_ptr<Table>, std::less<>> tables_;
};

} // namespace whist

#endif
