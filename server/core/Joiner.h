#ifndef WHIST_CORE_JOINER_H
#define WHIST_CORE_JOINER_H

#include "core/GameRun.h"

#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>

namespace whist
{

// Destroys finished and closed games on a thread of its own, because destroying one joins its
// game thread, and no I/O thread may ever wait on that.
class Joiner
{
public:
    Joiner();

    // Drains first.
    ~Joiner();

    Joiner(const Joiner&) = delete;
    Joiner& operator=(const Joiner&) = delete;

    // Any thread. Closes the run and queues it. Never blocks on a join.
    void hand(std::unique_ptr<GameRun> run);

    // Joins every run handed so far, then the joiner's own thread. Runs handed after this are
    // joined on the caller's thread, with an error logged, since nothing else is left to do it.
    void drain();

    // How many runs it has destroyed, for tests.
    std::size_t joined() const;

private:
    void work();

    mutable std::mutex mutex_;
    std::condition_variable changed_;
    std::deque<std::unique_ptr<GameRun>> queue_;
    std::size_t joined_ = 0;
    bool draining_ = false;
    bool drained_ = false;
    std::thread thread_;
};

} // namespace whist

#endif
