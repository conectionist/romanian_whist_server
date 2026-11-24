#include "core/RunningGames.h"

#include <utility>

namespace whist
{

RunningGames::RunningGames(std::size_t max, std::size_t maxInstant, std::size_t maxInstantPerAddress)
    : max_(max), maxInstant_(maxInstant), maxInstantPerAddress_(maxInstantPerAddress)
{
}

RunningGames::Slot::Slot(std::shared_ptr<RunningGames> games, bool instant, std::string address)
    : games_(std::move(games)), instant_(instant), address_(std::move(address))
{
}

RunningGames::Slot::Slot(Slot&& other) noexcept
    : games_(std::move(other.games_)),
      attended_(other.attended_),
      instant_(other.instant_),
      address_(std::move(other.address_)),
      countsForAddress_(other.countsForAddress_)
{
}

RunningGames::Slot& RunningGames::Slot::operator=(Slot&& other) noexcept
{
    if(this != &other)
    {
        release();
        games_ = std::move(other.games_);
        attended_ = other.attended_;
        instant_ = other.instant_;
        address_ = std::move(other.address_);
        countsForAddress_ = other.countsForAddress_;
    }
    return *this;
}

RunningGames::Slot::~Slot()
{
    release();
}

void RunningGames::Slot::setAttended(bool attended)
{
    if(!games_ || attended == attended_)
        return;

    std::lock_guard lock(games_->mutex_);
    attended_ = attended;
    if(attended)
    {
        games_->attended_++;
        if(instant_)
            games_->attendedInstant_++;
    }
    else
    {
        games_->attended_--;
        if(instant_)
            games_->attendedInstant_--;
    }
}

bool RunningGames::Slot::setInstant(bool instant, Full* full)
{
    if(!games_ || instant == instant_)
        return true;

    std::lock_guard lock(games_->mutex_);
    if(instant)
    {
        if(const std::optional<Full> refused =
               games_->instantFullLocked(countsForAddress_ ? address_ : std::string(), attended_))
        {
            if(full)
                *full = *refused;
            return false;
        }
    }

    if(attended_)
    {
        if(instant)
            games_->attendedInstant_++;
        else
            games_->attendedInstant_--;
    }
    if(countsForAddress_)
        games_->countAddressLocked(address_, instant);
    instant_ = instant;
    return true;
}

void RunningGames::Slot::leaveAddress()
{
    if(!games_ || !countsForAddress_)
        return;

    std::lock_guard lock(games_->mutex_);
    if(instant_)
        games_->countAddressLocked(address_, false);
    countsForAddress_ = false;
}

void RunningGames::Slot::release()
{
    if(const std::shared_ptr<RunningGames> games = std::move(games_))
    {
        std::lock_guard lock(games->mutex_);
        games->count_--;
        if(attended_)
        {
            games->attended_--;
            if(instant_)
                games->attendedInstant_--;
        }
        if(instant_ && countsForAddress_)
            games->countAddressLocked(address_, false);
    }
}

std::optional<RunningGames::Slot> RunningGames::take(bool instant, Full* full, std::string address)
{
    {
        std::lock_guard lock(mutex_);
        std::optional<Full> refused;
        if(max_ != 0 && attended_ >= max_)
            refused = Full::All;
        else if(instant)
            refused = instantFullLocked(address, true);
        if(refused)
        {
            if(full)
                *full = *refused;
            return std::nullopt;
        }

        count_++;
        attended_++;
        if(instant)
        {
            attendedInstant_++;
            countAddressLocked(address, true);
        }
    }
    return Slot(shared_from_this(), instant, std::move(address));
}

RunningGames::Counts RunningGames::counts() const
{
    std::lock_guard lock(mutex_);
    return {count_, attended_, attendedInstant_};
}

std::optional<RunningGames::Full> RunningGames::instantFullLocked(const std::string& address, bool attended) const
{
    if(attended && maxInstant_ != 0 && attendedInstant_ >= maxInstant_)
        return Full::Instant;
    if(maxInstantPerAddress_ != 0 && !address.empty())
    {
        const auto found = instantByAddress_.find(address);
        if(found != instantByAddress_.end() && found->second >= maxInstantPerAddress_)
            return Full::InstantAddress;
    }
    return std::nullopt;
}

void RunningGames::countAddressLocked(const std::string& address, bool more)
{
    if(address.empty())
        return;
    if(more)
        instantByAddress_[address]++;
    else if(const auto found = instantByAddress_.find(address);
            found != instantByAddress_.end() && --found->second == 0)
        instantByAddress_.erase(found);
}

std::size_t RunningGames::count() const
{
    std::lock_guard lock(mutex_);
    return count_;
}

std::size_t RunningGames::attended() const
{
    std::lock_guard lock(mutex_);
    return attended_;
}

std::size_t RunningGames::attendedInstant() const
{
    std::lock_guard lock(mutex_);
    return attendedInstant_;
}

std::size_t RunningGames::instantFrom(const std::string& address) const
{
    std::lock_guard lock(mutex_);
    const auto found = instantByAddress_.find(address);
    return found == instantByAddress_.end() ? 0 : found->second;
}

std::size_t RunningGames::max() const
{
    return max_;
}

std::size_t RunningGames::maxInstant() const
{
    return maxInstant_;
}

} // namespace whist
