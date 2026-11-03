#ifndef WHIST_CORE_REGISTRY_H
#define WHIST_CORE_REGISTRY_H

#include "core/Joiner.h"
#include "core/Outbox.h"
#include "core/Protocol.h"
#include "core/Settings.h"
#include "core/Table.h"

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
    // game starts at once.
    Created createSolo(MemberId member, const Settings& settings);

    std::shared_ptr<Table> findTable(std::string_view code) const;
    std::shared_ptr<Table> tableOf(MemberId member) const;
    std::size_t tableCount() const;

    void closeTable(std::string_view code);

    // Refuses new tables, closes every table, and joins every game thread. Call it before the
    // Outbox's connections are gone.
    void shutdown();

    const Joiner& joiner() const;

private:
    struct MemberRecord
    {
        std::string token;
        std::string table;
    };

    std::string newToken();
    std::string newCode();

    Outbox& outbox_;
    const TableOptions tableOptions_;
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
