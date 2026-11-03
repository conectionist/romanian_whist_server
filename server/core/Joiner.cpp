#include "core/Joiner.h"

#include "core/Log.h"
#include "core/Protocol.h"

#include <chrono>
#include <string>

namespace whist
{

Joiner::Joiner() : thread_([this] { work(); })
{
}

Joiner::~Joiner()
{
    drain();
}

void Joiner::hand(std::unique_ptr<GameRun> run)
{
    if(!run)
        return;

    run->close();
    {
        std::lock_guard lock(mutex_);
        if(!drained_)
        {
            queue_.push_back(std::move(run));
            changed_.notify_all();
            return;
        }
    }

    log(LogLevel::Error, "a game was handed to the joiner after it drained");
    run.reset();
}

void Joiner::drain()
{
    {
        std::lock_guard lock(mutex_);
        if(drained_)
            return;
        draining_ = true;
    }
    changed_.notify_all();

    if(thread_.joinable())
        thread_.join();
}

std::size_t Joiner::joined() const
{
    std::lock_guard lock(mutex_);
    return joined_;
}

void Joiner::work()
{
    std::unique_lock lock(mutex_);
    while(true)
    {
        changed_.wait(lock, [this] { return !queue_.empty() || draining_; });
        if(queue_.empty())
            break;

        std::unique_ptr<GameRun> run = std::move(queue_.front());
        queue_.pop_front();

        // Joined outside the lock, so hand() never waits behind a join.
        lock.unlock();
        const auto started = Clock::now();
        const unsigned int game = run->game();
        run.reset();
        const auto took = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - started);
        if(took > std::chrono::seconds(1))
            log(LogLevel::Warn, "joining game " + std::to_string(game) + " took " + std::to_string(took.count()) + " ms");
        lock.lock();
        joined_++;
    }
    drained_ = true;
}

} // namespace whist
