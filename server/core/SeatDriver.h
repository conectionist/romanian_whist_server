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

    // Called on the game thread, with no lock held, when a prompt for a member who is away opens or
    // closes, and when the seat is replaced there: the table has something new to tell everyone.
    using Changed = std::function<void()>;

    // A seat with a member asks that member, and its computer player (if any) stands in when a
    // turn times out or the member is gone. A seat with no member is played by its computer player
    // alone.
    SeatDriver(romanian_whist::Seat seat, std::optional<MemberId> member, ComputerPlayer computer,
               std::optional<std::chrono::milliseconds> turnTimer, Outbox& outbox,
               std::shared_ptr<std::atomic<std::uint64_t>> promptIds, Changed changed = {});

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

    // Any thread. The member's connection is gone: a prompt for them waits no later than
    // `graceEnds`, and a turn that comes after it is the computer player's. Does nothing for a seat
    // with no computer player to take its turns.
    void away(Clock::time_point graceEnds);

    // Any thread. The member's connection is back.
    void here();

    // Any thread. The computer player makes this seat's moves from now on, and a prompt open now is
    // closed as its own. A member who `silent`ly leaves the game is told nothing more.
    void replace(bool silent);

    // Any thread. The member has the seat again from the next decision; one already being made
    // finishes first. False when it was theirs already.
    bool reclaim();

    bool replaced() const;

    // The prompt open for a member who is away, and when it stops waiting for them.
    std::optional<Clock::time_point> awayDeadline() const;

    // Whether a human is asked for this seat's moves just now.
    bool humanInCharge() const;

    // Fixed for the game: the person whose seat this is, if it has one.
    std::optional<MemberId> member() const;
    romanian_whist::IGameObserver* observer() const;

    // A member whose turns time out this many times in a row is replaced.
    static constexpr int timeoutsBeforeReplacement = 3;

private:
    struct Pending
    {
        PromptKind kind;
        std::uint64_t value;
    };

    // Asks the member and returns their valid answer, or nothing when the deadline passed first.
    std::optional<std::uint64_t> ask(Prompt prompt, const std::function<bool(const Pending&)>& valid);

    // With the lock held: the earlier of the prompt's turn timer and, for a member who is away, the
    // end of their grace period.
    std::optional<Clock::time_point> deadlineOf(const Prompt& prompt) const;

    // Called without the lock, which it takes.
    void send(Json::Value message);

    void notifyTable() const;

    const romanian_whist::Seat seat_;
    const std::optional<MemberId> member_;
    const ComputerPlayer computer_;
    const std::optional<std::chrono::milliseconds> turnTimer_;
    Outbox& outbox_;
    const std::shared_ptr<std::atomic<std::uint64_t>> promptIds_;
    const romanian_whist::CardValidator validator_{};
    const Changed onChange_;

    mutable std::mutex mutex_;
    std::condition_variable changed_;
    std::optional<Prompt> open_;
    std::optional<Pending> answer_;
    std::uint64_t lastPrompt_ = 0;
    bool abandoned_ = false;

    // Counts every change of the three below, so a wait for a prompt's answer notices and works out
    // its deadline again.
    std::uint64_t version_ = 0;
    bool replaced_ = false;
    bool silent_ = false;
    std::optional<Clock::time_point> graceEnds_;
    int timedOut_ = 0;
};

} // namespace whist

#endif
