#include "core/Limits.h"

#include <algorithm>

namespace whist
{

namespace
{

constexpr std::chrono::minutes sweepEvery{1};
constexpr std::int64_t oneToken = 1'000'000;

// Longer than any bucket takes to fill, and short enough that the arithmetic cannot overflow.
constexpr std::chrono::hours longestRefill{24};

} // namespace

TokenBucket::TokenBucket(unsigned int perSecond, unsigned int burst, Clock::time_point now)
    : perMicrosecond_(perSecond), capacity_(std::int64_t{burst} * oneToken), tokens_(capacity_), last_(now)
{
}

bool TokenBucket::take(Clock::time_point now)
{
    if(perMicrosecond_ == 0)
        return true;

    // A clock that steps back adds nothing.
    if(now > last_)
    {
        const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
            std::min<Clock::duration>(now - last_, longestRefill));
        tokens_ = std::min(capacity_, tokens_ + elapsed.count() * perMicrosecond_);
        last_ = now;
    }
    if(tokens_ < oneToken)
        return false;

    tokens_ -= oneToken;
    return true;
}

Limits::Limits(LimitOptions options) : options_(std::move(options)), swept_(now())
{
}

Clock::time_point Limits::now() const
{
    return options_.clock ? options_.clock() : Clock::now();
}

TokenBucket Limits::messageBucket() const
{
    return TokenBucket(options_.messagesPerSecond, options_.messageBurst, now());
}

bool Limits::admitConnection(const std::string& address)
{
    if(address.empty() || options_.connectionsPerAddress == 0)
        return true;

    std::lock_guard lock(mutex_);
    const auto found = connections_.find(address);
    if(found != connections_.end() && found->second >= options_.connectionsPerAddress)
        return false;

    connections_[address]++;
    return true;
}

void Limits::releaseConnection(const std::string& address)
{
    if(address.empty() || options_.connectionsPerAddress == 0)
        return;

    std::lock_guard lock(mutex_);
    const auto found = connections_.find(address);
    if(found != connections_.end() && --found->second == 0)
        connections_.erase(found);
}

bool Limits::takeTable(const std::string& address)
{
    if(address.empty() || options_.tablesPerAddressPerHour == 0)
        return true;

    std::lock_guard lock(mutex_);
    const Clock::time_point at = now();
    forgetOldLocked(at);

    Times& times = tables_[address];
    expire(times, at, tableWindow);
    if(times.size() >= options_.tablesPerAddressPerHour)
        return false;

    times.push_back(at);
    return true;
}

void Limits::refundTable(const std::string& address)
{
    if(address.empty())
        return;

    std::lock_guard lock(mutex_);
    const auto found = tables_.find(address);
    if(found == tables_.end())
        return;

    if(!found->second.empty())
        found->second.pop_back();
    if(found->second.empty())
        tables_.erase(found);
}

bool Limits::joinsBlocked(const std::string& address)
{
    if(address.empty() || options_.failedJoinsPerAddress == 0)
        return false;

    std::lock_guard lock(mutex_);
    const Clock::time_point at = now();
    forgetOldLocked(at);

    const auto found = failedJoins_.find(address);
    if(found == failedJoins_.end())
        return false;

    expire(found->second, at, options_.failedJoinWindow);
    if(found->second.empty())
    {
        failedJoins_.erase(found);
        return false;
    }
    return found->second.size() >= options_.failedJoinsPerAddress;
}

void Limits::noteFailedJoin(const std::string& address)
{
    if(address.empty() || options_.failedJoinsPerAddress == 0)
        return;

    std::lock_guard lock(mutex_);
    const Clock::time_point at = now();
    Times& times = failedJoins_[address];
    expire(times, at, options_.failedJoinWindow);

    // Joins that race past the check could add more, and none past the limit is worth keeping.
    if(times.size() < options_.failedJoinsPerAddress)
        times.push_back(at);
}

std::size_t Limits::remembered() const
{
    std::lock_guard lock(mutex_);
    return connections_.size() + tables_.size() + failedJoins_.size();
}

void Limits::expire(Times& times, Clock::time_point now, std::chrono::milliseconds window)
{
    while(!times.empty() && now - times.front() >= window)
        times.pop_front();
}

// Addresses that never come back would otherwise stay for as long as the server runs.
void Limits::forgetOldLocked(Clock::time_point now)
{
    if(now - swept_ < sweepEvery)
        return;
    swept_ = now;

    const auto forget = [&](History& history, std::chrono::milliseconds window)
    {
        for(auto it = history.begin() ; it != history.end() ; )
        {
            expire(it->second, now, window);
            it = it->second.empty() ? history.erase(it) : std::next(it);
        }
    };
    forget(tables_, tableWindow);
    forget(failedJoins_, options_.failedJoinWindow);
}

} // namespace whist
