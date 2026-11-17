#ifndef WHIST_TESTS_CORE_MANUALCLOCK_H
#define WHIST_TESTS_CORE_MANUALCLOCK_H

#include "core/Limits.h"

#include <chrono>
#include <memory>
#include <mutex>

namespace whist::test
{

// Time that moves only when a test says so, so that no limit is ever waited for. Made with
// std::make_shared, since the limits it hands out keep it alive.
class ManualClock : public std::enable_shared_from_this<ManualClock>
{
public:
    Clock::time_point now() const
    {
        std::lock_guard lock(mutex_);
        return now_;
    }

    void advance(std::chrono::milliseconds by)
    {
        std::lock_guard lock(mutex_);
        now_ += by;
    }

    // Limits, all off, that read this clock.
    LimitOptions limits()
    {
        LimitOptions options;
        options.clock = [self = shared_from_this()] { return self->now(); };
        return options;
    }

private:
    mutable std::mutex mutex_;
    Clock::time_point now_ = Clock::now();
};

} // namespace whist::test

#endif
