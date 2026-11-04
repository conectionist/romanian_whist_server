#ifndef WHIST_CORE_SEATDRIVER_H
#define WHIST_CORE_SEATDRIVER_H

#include "core/ComputerPlayer.h"
#include "core/Outbox.h"
#include "core/Protocol.h"

#include <romanian_whist/BetContext.h>
#include <romanian_whist/CardValidator.h>
#include <romanian_whist/PlayContext.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>

namespace whist
{

// One seat's player. The engine asks it for moves on the game thread; a human's answers reach it
// from I/O threads through submit().
class SeatDriver
{
public:
    enum class Submitted
    {
        Taken,
        Stale,
        NotYourTurn
    };

    // A seat with a member asks that member, and its computer player (if any) stands in when a
    // turn times out. A seat with no member is played by its computer player alone.
    SeatDriver(romanian_whist::Seat seat, std::optional<MemberId> member, ComputerPlayer computer,
               std::optional<std::chrono::milliseconds> turnTimer, Outbox& outbox,
               std::shared_ptr<std::atomic<std::uint64_t>> promptIds);

    SeatDriver(const SeatDriver&) = delete;
    SeatDriver& operator=(const SeatDriver&) = delete;

    // Game thread. `blind` is whether the seat may not see its own hand this round.
    unsigned int makeBet(const romanian_whist::BetContext& context, bool blind);
    std::optional<std::size_t> playCard(const romanian_whist::PlayContext& context, bool blind);

    // Any thread. Only gates: a prompt must be open, with this id, and not answered yet. Whether
    // the answer is legal is the game thread's call.
    Submitted submit(const Answer& answer);

    // Any thread, at any time. A waiting seat throws GameAbandoned; a later one throws at once. Once
    // it returns, the seat sends nothing more.
    void abandon();

    // Calls `use` with the open prompt, under this seat's lock, so the prompt cannot close while a
    // sync that carries it is sent.
    void withOpenPrompt(const std::function<void(const std::optional<Prompt>&)>& use) const;

    // Fixed for the game: whether a human is asked for this seat's moves.
    bool humanInCharge() const;
    std::optional<MemberId> member() const;
    romanian_whist::IGameObserver* observer() const;

private:
    struct Pending
    {
        PromptKind kind;
        std::uint64_t value;
    };

    // Asks the member and returns their valid answer, or nothing when the deadline passed first.
    std::optional<std::uint64_t> ask(Prompt prompt, const std::function<bool(const Pending&)>& valid);

    // Called without the lock, which it takes.
    void send(Json::Value message);

    const romanian_whist::Seat seat_;
    const std::optional<MemberId> member_;
    const ComputerPlayer computer_;
    const std::optional<std::chrono::milliseconds> turnTimer_;
    Outbox& outbox_;
    const std::shared_ptr<std::atomic<std::uint64_t>> promptIds_;
    const romanian_whist::CardValidator validator_{};

    mutable std::mutex mutex_;
    std::condition_variable changed_;
    std::optional<Prompt> open_;
    std::optional<Pending> answer_;
    std::uint64_t lastPrompt_ = 0;
    bool abandoned_ = false;
};

} // namespace whist

#endif
