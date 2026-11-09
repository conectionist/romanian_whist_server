#include "core/Seating.h"

#include "core/Catalogue.h"

#include <algorithm>
#include <utility>

namespace whist
{

Seating::Seating(Settings settings, std::vector<TableMember> members, MemberId host)
    : settings_(std::move(settings)),
      members_(std::move(members)),
      host_(host)
{
}

const Settings& Seating::settings() const
{
    return settings_;
}

const std::vector<TableMember>& Seating::members() const
{
    return members_;
}

MemberId Seating::host() const
{
    return host_;
}

bool Seating::hasMember(MemberId member) const
{
    return std::ranges::any_of(members_, [member](const TableMember& m) { return m.id == member; });
}

std::optional<unsigned int> Seating::seatOf(MemberId member) const
{
    for(const TableMember& m : members_)
    {
        if(m.id == member)
            return m.seat;
    }
    return std::nullopt;
}

std::optional<MemberId> Seating::memberAt(unsigned int seat) const
{
    for(const TableMember& m : members_)
    {
        if(m.seat == seat)
            return m.id;
    }
    return std::nullopt;
}

bool Seating::hasOpenSeat() const
{
    return std::ranges::any_of(settings_.seats, [](const SeatSetting& seat) { return seat.kind == SeatKind::Open; });
}

TableMember* Seating::find(MemberId member)
{
    const auto found = std::ranges::find_if(members_, [member](const TableMember& m) { return m.id == member; });
    return found == members_.end() ? nullptr : &*found;
}

void Seating::setPacing(int botSpeed, bool waitAfterRound)
{
    settings_.botSpeed = botSpeed;
    settings_.waitAfterRound = waitAfterRound;
}

std::optional<Clock::time_point> Seating::awaySince(MemberId member) const
{
    for(const TableMember& m : members_)
    {
        if(m.id == member)
            return m.awaySince;
    }
    return std::nullopt;
}

void Seating::setAway(MemberId member, std::optional<Clock::time_point> since)
{
    if(TableMember* found = find(member))
        found->awaySince = since;
}

bool Seating::passHost(const std::function<bool(MemberId)>& here)
{
    for(const TableMember& m : members_)
    {
        if(m.id != host_ && here(m.id))
        {
            host_ = m.id;
            return true;
        }
    }
    return false;
}

void Seating::depart(MemberId member)
{
    const auto found = std::ranges::find_if(members_, [member](const TableMember& m) { return m.id == member; });
    if(found == members_.end())
        return;

    TableMember leaving = *found;
    leaving.awaySince.reset();
    departed_.push_back(leaving);
    members_.erase(found);

    if(host_ == member && !members_.empty())
        host_ = members_.front().id;
}

bool Seating::hasDeparted() const
{
    return !departed_.empty();
}

bool Seating::departed(MemberId member) const
{
    return std::ranges::any_of(departed_, [member](const TableMember& m) { return m.id == member; });
}

void Seating::rejoin(MemberId member)
{
    const auto found = std::ranges::find_if(departed_, [member](const TableMember& m) { return m.id == member; });
    if(found == departed_.end())
        return;

    members_.push_back(*found);
    departed_.erase(found);
}

void Seating::openDeparted()
{
    for(const TableMember& m : departed_)
    {
        if(m.seat)
            settings_.seats.at(*m.seat) = SeatSetting{};
    }
    departed_.clear();
}

std::set<std::string> Seating::takenKeys(std::optional<unsigned int> except) const
{
    std::set<std::string> keys;
    for(unsigned int i = 0 ; i < settings_.seats.size() ; i++)
    {
        if(i != except && settings_.seats[i].kind != SeatKind::Open)
            keys.insert(nameKey(settings_.seats[i].profile.name));
    }
    return keys;
}

std::map<std::string, unsigned int> Seating::worn(std::optional<unsigned int> except) const
{
    std::map<std::string, unsigned int> counts;
    for(unsigned int i = 0 ; i < settings_.seats.size() ; i++)
    {
        if(i != except && settings_.seats[i].kind != SeatKind::Open)
            counts[settings_.seats[i].profile.avatar]++;
    }
    return counts;
}

std::optional<Refusal> Seating::join(MemberId member, Profile profile)
{
    const auto open = std::ranges::find_if(settings_.seats, [](const SeatSetting& seat)
    {
        return seat.kind == SeatKind::Open;
    });
    if(open == settings_.seats.end())
        return ErrorCode::TableFull;

    const auto seat = static_cast<unsigned int>(open - settings_.seats.begin());
    profile.name = numberedName(profile.name, takenKeys(std::nullopt));
    open->kind = SeatKind::Human;
    open->level.reset();
    open->profile = std::move(profile);
    members_.push_back({member, seat});
    return std::nullopt;
}

void Seating::leave(MemberId member)
{
    const auto found = std::ranges::find_if(members_, [member](const TableMember& m) { return m.id == member; });
    if(found == members_.end())
        return;

    if(found->seat)
        settings_.seats.at(*found->seat) = SeatSetting{};
    members_.erase(found);

    if(host_ == member && !members_.empty())
        host_ = members_.front().id;
}

std::optional<Refusal> Seating::takeSeat(MemberId member, unsigned int seat)
{
    TableMember* moving = find(member);
    if(moving == nullptr)
        return ErrorCode::NotFound;
    if(seat >= settings_.seats.size())
        return Refusal(ErrorCode::BadMessage, "seat");
    if(moving->seat == seat)
        return std::nullopt;
    if(settings_.seats[seat].kind != SeatKind::Open)
        return ErrorCode::Illegal;

    if(!moving->seat)
        return ErrorCode::Illegal;

    settings_.seats[seat] = std::exchange(settings_.seats[*moving->seat], SeatSetting{});
    moving->seat = seat;
    return std::nullopt;
}

std::optional<Refusal> Seating::setSeat(MemberId by, unsigned int seat, SeatKind kind, std::optional<Level> level,
                                        std::string_view language, std::mt19937_64& random)
{
    if(by != host_)
        return ErrorCode::NotHost;
    if(seat >= settings_.seats.size())
        return Refusal(ErrorCode::BadMessage, "seat");

    SeatSetting& setting = settings_.seats[seat];
    if(setting.kind == SeatKind::Human)
        return ErrorCode::Illegal;

    if(kind == SeatKind::Open)
    {
        setting = SeatSetting{};
        return std::nullopt;
    }
    if(kind != SeatKind::Computer || !level)
        return Refusal(ErrorCode::BadMessage, "kind");
    if(setting.kind == SeatKind::Computer && setting.level == level)
        return std::nullopt;

    std::optional<std::string> gender;
    if(setting.kind == SeatKind::Computer)
        gender = setting.profile.gender;

    SeatSetting computer;
    computer.kind = SeatKind::Computer;
    computer.level = level;
    computer.profile = Catalogue::get().drawComputer(*level, language, gender, seat + 1, takenKeys(seat), worn(seat),
                                                     random);
    setting = std::move(computer);
    return std::nullopt;
}

std::optional<Refusal> Seating::updateProfile(MemberId member, Profile profile)
{
    const std::optional<unsigned int> seat = seatOf(member);
    if(!seat)
        return ErrorCode::Illegal;
    if(takenKeys(seat).contains(nameKey(profile.name)))
        return ErrorCode::NameTaken;

    settings_.seats[*seat].profile = std::move(profile);
    return std::nullopt;
}

std::optional<Refusal> Seating::updateSettings(MemberId by, const Settings& settings)
{
    if(by != host_)
        return ErrorCode::NotHost;
    Settings next = settings;
    completeOnline(next);
    if(const std::optional<SettingsError> error = checkOnlineRules(next))
        return Refusal(ErrorCode::BadMessage, error->field);

    const std::size_t count = settings.seats.size();
    for(std::size_t i = count ; i < settings_.seats.size() ; i++)
    {
        if(settings_.seats[i].kind == SeatKind::Human)
            return Refusal(ErrorCode::Illegal, "seats");
    }

    next.seats = settings_.seats;
    next.seats.resize(count);
    settings_ = std::move(next);
    return std::nullopt;
}

} // namespace whist
