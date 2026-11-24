#include "core/RunningGames.h"

#include <utility>

namespace whist
{

RunningGames::RunningGames(std::size_t max, std::size_t maxInstant) : max_(max), maxInstant_(maxInstant)
{
}

RunningGames::Slot::Slot(std::shared_ptr<RunningGames> games, bool instant) : games_(std::move(games)), instant_(instant)
{
}

RunningGames::Slot::Slot(Slot&& other) noexcept
    : games_(std::move(other.games_)), attended_(other.attended_), instant_(other.instant_)
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

bool RunningGames::Slot::setInstant(bool instant)
{
    if(!games_ || instant == instant_)
        return true;

    std::lock_guard lock(games_->mutex_);
    if(attended_)
    {
        if(instant && games_->instantFullLocked())
            return false;
        if(instant)
            games_->attendedInstant_++;
        else
            games_->attendedInstant_--;
    }
    instant_ = instant;
    return true;
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
    }
}

std::optional<RunningGames::Slot> RunningGames::take(bool instant)
{
    {
        std::lock_guard lock(mutex_);
        if(max_ != 0 && attended_ >= max_)
            return std::nullopt;
        if(instant && instantFullLocked())
            return std::nullopt;

        count_++;
        attended_++;
        if(instant)
            attendedInstant_++;
    }
    return Slot(shared_from_this(), instant);
}

bool RunningGames::instantFullLocked() const
{
    return maxInstant_ != 0 && attendedInstant_ >= maxInstant_;
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

std::size_t RunningGames::max() const
{
    return max_;
}

std::size_t RunningGames::maxInstant() const
{
    return maxInstant_;
}

} // namespace whist
