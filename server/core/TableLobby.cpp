#include "core/Table.h"

#include <utility>

namespace whist
{

std::optional<Refusal> Table::checkBetweenGamesLocked(MemberId by) const
{
    if(state_ == TableState::Closed)
        return ErrorCode::NotFound;
    if(!seating_.hasMember(by))
        return ErrorCode::NotFound;
    if(mode_ != Mode::Online)
        return ErrorCode::Illegal;

    // A finished table is a lobby again until the next game starts.
    if(stateLocked() == TableState::Playing)
        return ErrorCode::Illegal;
    return std::nullopt;
}

std::optional<Refusal> Table::join(MemberId member, Profile profile)
{
    std::lock_guard lock(mutex_);
    if(state_ == TableState::Closed || mode_ != Mode::Online)
        return ErrorCode::NotFound;
    if(seating_.hasMember(member))
        return ErrorCode::Illegal;

    settleLocked();

    // Someone who left a game still under way comes back to their seat, whatever profile they bring:
    // the engine fixed the names when it started.
    if(seating_.departed(member))
    {
        seating_.rejoin(member);
        arriveLocked(member);
        syncLocked(member);
        broadcastLocked(member);
        return std::nullopt;
    }

    // A table with a game on has no open seat.
    if(std::optional<Refusal> refused = seating_.join(member, std::move(profile)))
        return refused;

    refreshPresenceLocked();
    syncLocked(member);
    broadcastLocked(member);
    return std::nullopt;
}

Table::Left Table::leave(MemberId member)
{
    std::lock_guard lock(mutex_);
    if(state_ == TableState::Closed || !seating_.hasMember(member))
        return {ErrorCode::NotFound, false};
    if(mode_ != Mode::Online)
        return {ErrorCode::Illegal, false};

    settleLocked();

    if(run_ && stateLocked() == TableState::Playing && seating_.seatOf(member))
    {
        // Their seat stays theirs on the table, and the game goes on with a computer player in it.
        seating_.depart(member);
        run_->replace(member, true);
    }
    else
        seating_.leave(member);

    refreshPresenceLocked();
    if(seating_.members().empty())
        return {std::nullopt, true};

    broadcastLocked();
    return {std::nullopt, false};
}

std::optional<Refusal> Table::takeSeat(MemberId member, unsigned int seat)
{
    std::lock_guard lock(mutex_);
    settleLocked();
    if(std::optional<Refusal> refused = checkBetweenGamesLocked(member))
        return refused;
    if(std::optional<Refusal> refused = seating_.takeSeat(member, seat))
        return refused;
    broadcastLocked();
    return std::nullopt;
}

std::optional<Refusal> Table::setSeat(MemberId by, unsigned int seat, SeatKind kind, std::optional<Level> level,
                                      std::string_view language)
{
    std::lock_guard lock(mutex_);
    settleLocked();
    if(std::optional<Refusal> refused = checkBetweenGamesLocked(by))
        return refused;
    if(std::optional<Refusal> refused = seating_.setSeat(by, seat, kind, level, language, random_))
        return refused;
    broadcastLocked();
    return std::nullopt;
}

std::optional<Refusal> Table::updateProfile(MemberId member, Profile profile)
{
    if(const std::optional<SettingsError> error = preparePersonProfile(profile))
        return Refusal(ErrorCode::BadMessage, error->field);

    std::lock_guard lock(mutex_);
    settleLocked();
    if(std::optional<Refusal> refused = checkBetweenGamesLocked(member))
        return refused;
    if(std::optional<Refusal> refused = seating_.updateProfile(member, std::move(profile)))
        return refused;
    broadcastLocked();
    return std::nullopt;
}

void Table::presenceChanged(MemberId member)
{
    std::lock_guard lock(mutex_);
    if(state_ == TableState::Closed || !seating_.hasMember(member))
        return;

    // Told late of a connection that a newer tab has since replaced: nothing has changed.
    if(outbox_.isConnected(member))
        return;

    if(!seating_.awaySince(member))
        seating_.setAway(member, Clock::now());
    refreshPresenceLocked();

    // A solo game never replaces anyone, and nobody else is there to be told.
    if(mode_ != Mode::Online)
        return;

    if(run_ && stateLocked() == TableState::Playing)
    {
        run_->away(member, *seating_.awaySince(member) + options_.gracePeriod);
        run_->excuse(member);
    }
    broadcastLocked();
}

} // namespace whist
