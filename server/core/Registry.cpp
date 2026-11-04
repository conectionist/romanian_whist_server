#include "core/Registry.h"

#include <array>
#include <random>
#include <utility>
#include <vector>

namespace whist
{

namespace
{

// No 0/O, 1/I/L: a code is read aloud and typed.
constexpr std::string_view codeAlphabet = "ABCDEFGHJKMNPQRSTUVWXYZ23456789";
constexpr std::size_t codeLength = 6;

constexpr std::string_view base64Url = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

std::function<std::uint64_t()> systemRandom()
{
    auto device = std::make_shared<std::random_device>();
    return [device]
    {
        return (static_cast<std::uint64_t>((*device)()) << 32) | static_cast<std::uint64_t>((*device)());
    };
}

} // namespace

Registry::Registry(Outbox& outbox, RegistryOptions options)
    : outbox_(outbox),
      tableOptions_(options.table),
      random_(options.random ? std::move(options.random) : systemRandom())
{
}

Registry::~Registry()
{
    shutdown();
}

std::string Registry::newToken()
{
    std::array<std::uint8_t, 16> bytes{};
    for(std::size_t i = 0 ; i < bytes.size() ; i += 8)
    {
        const std::uint64_t word = random_();
        for(std::size_t j = 0 ; j < 8 ; j++)
            bytes[i + j] = static_cast<std::uint8_t>(word >> (8 * j));
    }

    // Base64url without padding: 128 bits make 22 characters.
    std::string token;
    std::uint32_t buffer = 0;
    int bits = 0;
    for(std::uint8_t byte : bytes)
    {
        buffer = (buffer << 8) | byte;
        bits += 8;
        while(bits >= 6)
        {
            bits -= 6;
            token += base64Url[(buffer >> bits) & 0x3F];
        }
    }
    if(bits > 0)
        token += base64Url[(buffer << (6 - bits)) & 0x3F];
    return token;
}

std::string Registry::newCode()
{
    // Rejection sampling keeps every character equally likely.
    const std::uint64_t limit = (std::uint64_t{1} << 63) / codeAlphabet.size() * codeAlphabet.size();
    std::string code;
    while(code.size() < codeLength)
    {
        const std::uint64_t draw = random_() >> 1;
        if(draw < limit)
            code += codeAlphabet[draw % codeAlphabet.size()];
    }
    return code;
}

Registry::NewMember Registry::newMember()
{
    std::lock_guard lock(mutex_);
    std::string token;
    do
        token = newToken();
    while(tokens_.contains(token));

    const MemberId id = nextMember_++;
    members_[id] = MemberRecord{token, {}};
    tokens_[token] = id;
    return {id, token};
}

std::optional<MemberId> Registry::findMember(std::string_view token) const
{
    std::lock_guard lock(mutex_);
    const auto found = tokens_.find(std::string(token));
    if(found == tokens_.end())
        return std::nullopt;
    return found->second;
}

Registry::Created Registry::createSolo(MemberId member, Settings settings)
{
    if(const std::optional<SettingsError> error = cleanNames(settings))
        return {nullptr, ErrorCode::BadMessage, error->field};
    if(const std::optional<SettingsError> error = checkSettings(settings, Mode::Solo))
        return {nullptr, ErrorCode::BadMessage, error->field};

    std::shared_ptr<Table> table;
    {
        std::lock_guard lock(mutex_);
        if(closing_)
            return {nullptr, ErrorCode::Draining, {}};

        const auto record = members_.find(member);
        if(record == members_.end())
            return {nullptr, ErrorCode::NotFound, {}};
        if(!record->second.table.empty())
            return {nullptr, ErrorCode::Illegal, {}};

        std::optional<unsigned int> seat;
        for(unsigned int i = 0 ; i < settings.seats.size() ; i++)
        {
            if(settings.seats[i].kind == SeatKind::Human)
                seat = i;
        }

        std::string code;
        do
            code = newCode();
        while(tables_.contains(code));

        table = std::make_shared<Table>(code, Mode::Solo, Seating(std::move(settings), {{member, seat}}, member),
                                        tableOptions_, outbox_, joiner_, random_());
        tables_[code] = table;
        record->second.table = code;

        if(const std::optional<Refusal> refused = table->startGame(member))
        {
            tables_.erase(code);
            record->second.table.clear();
            return {nullptr, refused->code, refused->detail};
        }
    }
    return {table, std::nullopt, {}};
}

Registry::Created Registry::createTable(MemberId member, Settings settings, Profile profile)
{
    if(const std::optional<SettingsError> error = cleanNames(settings))
        return {nullptr, ErrorCode::BadMessage, error->field};
    if(const std::optional<SettingsError> error = checkNewTableSettings(settings))
        return {nullptr, ErrorCode::BadMessage, error->field};
    if(const std::optional<SettingsError> error = preparePersonProfile(profile))
        return {nullptr, ErrorCode::BadMessage, error->field};
    completeOnline(settings);

    // The maker chose their computer players' names, so a clash with their own is theirs to fix.
    for(const SeatSetting& seat : settings.seats)
    {
        if(seat.kind == SeatKind::Computer && nameKey(seat.profile.name) == nameKey(profile.name))
            return {nullptr, ErrorCode::BadMessage, "name"};
    }

    std::shared_ptr<Table> table;
    {
        std::lock_guard lock(mutex_);
        if(closing_)
            return {nullptr, ErrorCode::Draining, {}};

        const auto record = members_.find(member);
        if(record == members_.end())
            return {nullptr, ErrorCode::NotFound, {}};
        if(!record->second.table.empty())
            return {nullptr, ErrorCode::Illegal, {}};

        std::string code;
        do
            code = newCode();
        while(tables_.contains(code));

        Seating seating(std::move(settings), {}, member);
        seating.join(member, std::move(profile));
        table = std::make_shared<Table>(code, Mode::Online, std::move(seating), tableOptions_, outbox_, joiner_,
                                        random_());
        tables_[code] = table;
        record->second.table = code;

        // Sent under this lock, so nothing the table sends can come before it.
        table->sync(member);
    }
    return {table, std::nullopt, {}};
}

std::optional<Refusal> Registry::joinTable(MemberId member, std::string_view code, Profile profile)
{
    if(const std::optional<SettingsError> error = preparePersonProfile(profile))
        return Refusal(ErrorCode::BadMessage, error->field);

    std::lock_guard lock(mutex_);
    if(closing_)
        return ErrorCode::Draining;

    const auto record = members_.find(member);
    if(record == members_.end())
        return ErrorCode::NotFound;
    if(!record->second.table.empty())
        return ErrorCode::Illegal;

    // A solo table's code is no way in: it is not found.
    const auto found = tables_.find(code);
    if(found == tables_.end() || found->second->mode() != Mode::Online)
        return ErrorCode::NotFound;

    if(std::optional<Refusal> refused = found->second->join(member, std::move(profile)))
        return refused;
    record->second.table = found->first;
    return std::nullopt;
}

std::optional<Refusal> Registry::leaveTable(MemberId member)
{
    std::shared_ptr<Table> closing;
    {
        std::lock_guard lock(mutex_);
        const auto record = members_.find(member);
        if(record == members_.end() || record->second.table.empty())
            return ErrorCode::Illegal;

        const auto found = tables_.find(record->second.table);
        if(found == tables_.end())
            return ErrorCode::Illegal;
        const std::shared_ptr<Table> table = found->second;

        if(table->mode() == Mode::Solo)
        {
            // The member was the solo table's only person, so leaving it closes it.
            closing = removeLocked(table->code());
        }
        else
        {
            const Table::Left left = table->leave(member);
            if(left.refused)
                return left.refused;
            record->second.table.clear();
            if(left.empty)
                closing = removeLocked(table->code());
        }
    }

    if(closing)
        closing->close();
    return std::nullopt;
}

std::shared_ptr<Table> Registry::removeLocked(std::string_view code)
{
    const auto found = tables_.find(code);
    if(found == tables_.end())
        return nullptr;
    std::shared_ptr<Table> table = found->second;
    tables_.erase(found);

    for(MemberId member : table->members())
    {
        const auto record = members_.find(member);
        if(record != members_.end() && record->second.table == table->code())
            record->second.table.clear();
    }
    return table;
}

std::shared_ptr<Table> Registry::findTable(std::string_view code) const
{
    std::lock_guard lock(mutex_);
    const auto found = tables_.find(code);
    return found == tables_.end() ? nullptr : found->second;
}

std::shared_ptr<Table> Registry::tableOf(MemberId member) const
{
    std::lock_guard lock(mutex_);
    const auto record = members_.find(member);
    if(record == members_.end() || record->second.table.empty())
        return nullptr;
    const auto found = tables_.find(record->second.table);
    return found == tables_.end() ? nullptr : found->second;
}

std::size_t Registry::tableCount() const
{
    std::lock_guard lock(mutex_);
    return tables_.size();
}

void Registry::closeTable(std::string_view code)
{
    std::shared_ptr<Table> table;
    {
        std::lock_guard lock(mutex_);
        table = removeLocked(code);
    }
    if(table)
        table->close();
}

void Registry::shutdown()
{
    std::vector<std::shared_ptr<Table>> tables;
    {
        std::lock_guard lock(mutex_);
        closing_ = true;
        for(auto& [code, table] : tables_)
            tables.push_back(table);
        tables_.clear();
        for(auto& [id, record] : members_)
            record.table.clear();
    }

    for(const std::shared_ptr<Table>& table : tables)
        table->close();
    joiner_->drain();
}

const Joiner& Registry::joiner() const
{
    return *joiner_;
}

} // namespace whist
