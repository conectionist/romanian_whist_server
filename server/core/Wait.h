#ifndef WHIST_CORE_WAIT_H
#define WHIST_CORE_WAIT_H

#include "core/Protocol.h"

#include <condition_variable>
#include <mutex>
#include <optional>

namespace whist
{

// The one way a game thread waits: until `done` holds, or the deadline passes when there is one.
// Returns `done()`. A build that cannot block a thread replaces this function and nothing else.
template<class Done>
bool waitUntil(std::condition_variable& changed, std::unique_lock<std::mutex>& lock,
               std::optional<Clock::time_point> deadline, Done done)
{
    if(!deadline)
    {
        changed.wait(lock, done);
        return true;
    }
    return changed.wait_until(lock, *deadline, done);
}

} // namespace whist

#endif
