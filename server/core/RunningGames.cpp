#include "core/RunningGames.h"

#include <utility>

namespace whist
{

RunningGames::RunningGames(std::size_t max) : max_(max)
{
}

RunningGames::Slot::Slot(std::shared_ptr<RunningGames> games) : games_(std::move(games))
{
}

RunningGames::Slot::Slot(Slot&& other) noexcept : games_(std::move(other.games_)), attended_(other.attended_)
{
}

RunningGames::Slot& RunningGames::Slot::operator=(Slot&& other) noexcept
{
    if(this != &other)
    {
        release();
        games_ = std::move(other.games_);
        attended_ = other.attended_;
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

    attended_ = attended;
    if(attended)
        games_->attended_++;
    else
        games_->attended_--;
}

void RunningGames::Slot::release()
{
    if(const std::shared_ptr<RunningGames> games = std::move(games_))
    {
        games->count_--;
        if(attended_)
            games->attended_--;
    }
}

std::optional<RunningGames::Slot> RunningGames::take()
{
    std::size_t seen = attended_.load();
    do
    {
        if(max_ != 0 && seen >= max_)
            return std::nullopt;
    }
    while(!attended_.compare_exchange_weak(seen, seen + 1));

    count_++;
    return Slot(shared_from_this());
}

std::size_t RunningGames::count() const
{
    return count_.load();
}

std::size_t RunningGames::attended() const
{
    return attended_.load();
}

std::size_t RunningGames::max() const
{
    return max_;
}

} // namespace whist
