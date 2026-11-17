#ifndef WHIST_CORE_RUNNINGGAMES_H
#define WHIST_CORE_RUNNINGGAMES_H

#include <atomic>
#include <cstddef>
#include <memory>
#include <optional>

namespace whist
{

// The game threads running, and the games among them that count against the most that may run: those
// someone is at. A game nobody is at holds and uses no processor, so it is no reason to refuse another.
// A game takes a Slot before its thread starts and gives it back when the thread is done with the engine.
class RunningGames : public std::enable_shared_from_this<RunningGames>
{
public:
    // `max` 0 allows any number of attended games.
    explicit RunningGames(std::size_t max);

    class Slot
    {
    public:
        Slot(Slot&& other) noexcept;
        Slot& operator=(Slot&& other) noexcept;
        ~Slot();

        Slot(const Slot&) = delete;
        Slot& operator=(const Slot&) = delete;

        // Whether the game counts against the most. A slot is attended when it is taken. Making it
        // attended again is never refused, so a player who comes back always has their game. One
        // thread at a time.
        void setAttended(bool attended);

        // Gives the place back at once. Safe to call again.
        void release();

    private:
        friend class RunningGames;
        explicit Slot(std::shared_ptr<RunningGames> games);

        std::shared_ptr<RunningGames> games_;
        bool attended_ = true;
    };

    // A place for one more game, or nothing when `max` attended games are running.
    std::optional<Slot> take();

    // The game threads running.
    std::size_t count() const;

    // The games that count against the most.
    std::size_t attended() const;

    std::size_t max() const;

private:
    const std::size_t max_;
    std::atomic<std::size_t> count_{0};
    std::atomic<std::size_t> attended_{0};
};

} // namespace whist

#endif
