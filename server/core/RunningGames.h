#ifndef WHIST_CORE_RUNNINGGAMES_H
#define WHIST_CORE_RUNNINGGAMES_H

#include <atomic>
#include <cstddef>
#include <memory>
#include <optional>

namespace whist
{

// How many game threads are running, and the most that may. A game takes a Slot before its thread
// starts and gives it back when the thread is done with the engine.
class RunningGames : public std::enable_shared_from_this<RunningGames>
{
public:
    // `max` 0 allows any number.
    explicit RunningGames(std::size_t max);

    class Slot
    {
    public:
        Slot(Slot&& other) noexcept;
        Slot& operator=(Slot&& other) noexcept;
        ~Slot();

        Slot(const Slot&) = delete;
        Slot& operator=(const Slot&) = delete;

        // Gives the place back at once. Safe to call again.
        void release();

    private:
        friend class RunningGames;
        explicit Slot(std::shared_ptr<RunningGames> games);

        std::shared_ptr<RunningGames> games_;
    };

    // A place for one more game, or nothing when `max` are running.
    std::optional<Slot> take();

    std::size_t count() const;
    std::size_t max() const;

private:
    const std::size_t max_;
    std::atomic<std::size_t> count_{0};
};

} // namespace whist

#endif
