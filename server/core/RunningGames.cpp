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

RunningGames::Slot::Slot(Slot&& other) noexcept : games_(std::move(other.games_))
{
}

RunningGames::Slot& RunningGames::Slot::operator=(Slot&& other) noexcept
{
    if(this != &other)
    {
        release();
        games_ = std::move(other.games_);
    }
    return *this;
}

RunningGames::Slot::~Slot()
{
    release();
}

void RunningGames::Slot::release()
{
    if(const std::shared_ptr<RunningGames> games = std::move(games_))
        games->count_--;
}

std::optional<RunningGames::Slot> RunningGames::take()
{
    std::size_t seen = count_.load();
    do
    {
        if(max_ != 0 && seen >= max_)
            return std::nullopt;
    }
    while(!count_.compare_exchange_weak(seen, seen + 1));

    return Slot(shared_from_this());
}

std::size_t RunningGames::count() const
{
    return count_.load();
}

std::size_t RunningGames::max() const
{
    return max_;
}

} // namespace whist
