#include "core/SeatDriver.h"

#include "core/Codec.h"
#include "core/GameAbandoned.h"
#include "core/Wait.h"

#include <algorithm>
#include <utility>

namespace whist
{

using namespace romanian_whist;

SeatDriver::SeatDriver(Seat seat, std::optional<MemberId> member, ComputerPlayer computer,
                       std::optional<std::chrono::milliseconds> turnTimer, Outbox& outbox,
                       std::shared_ptr<std::atomic<std::uint64_t>> promptIds, Changed changed)
    : seat_(seat),
      member_(member),
      computer_(std::move(computer)),
      turnTimer_(turnTimer),
      outbox_(outbox),
      promptIds_(std::move(promptIds)),
      onChange_(std::move(changed))
{
}

unsigned int SeatDriver::makeBet(const BetContext& context, bool blind)
{
    if(!member_)
        return computer_.provider->makeBet(context);

    Prompt prompt;
    prompt.kind = PromptKind::Bet;
    prompt.blind = blind;
    prompt.handSize = context.hand.size();
    if(!blind)
        prompt.hand = context.hand;
    prompt.max = static_cast<unsigned int>(context.hand.size());
    prompt.forbidden = context.forbiddenBet;
    prompt.first = context.isFirstPlayer;
    prompt.roundType = context.roundType;

    // Still 64 bits wide here, so a huge answer cannot wrap to a legal bid.
    const auto valid = [&](const Pending& answer)
    {
        return answer.kind == PromptKind::Bet && answer.value <= context.hand.size() &&
               (!context.forbiddenBet || answer.value != *context.forbiddenBet);
    };

    if(const std::optional<std::uint64_t> answer = ask(std::move(prompt), valid))
        return static_cast<unsigned int>(*answer);
    return computer_.provider->makeBet(context);
}

std::optional<std::size_t> SeatDriver::playCard(const PlayContext& context, bool blind)
{
    if(!member_)
        return computer_.provider->playCard(context);

    const std::vector<Card> legalCards = validator_.getLegalCards(context.hand, context.trump, context.leadSuit);
    if(legalCards.empty())
        return std::nullopt;

    Prompt prompt;
    prompt.kind = PromptKind::Play;
    prompt.blind = blind;
    prompt.handSize = context.hand.size();
    if(!blind)
        prompt.hand = context.hand;
    for(std::size_t i = 0 ; i < context.hand.size() ; i++)
    {
        if(std::ranges::find(legalCards, context.hand[i]) != legalCards.end())
            prompt.legal.push_back(i);
    }

    const std::vector<std::size_t> legal = prompt.legal;
    const auto valid = [&](const Pending& answer)
    {
        return answer.kind == PromptKind::Play && std::ranges::find(legal, answer.value) != legal.end();
    };

    if(const std::optional<std::uint64_t> answer = ask(std::move(prompt), valid))
        return static_cast<std::size_t>(*answer);
    return computer_.provider->playCard(context);
}

std::optional<std::uint64_t> SeatDriver::ask(Prompt prompt, const std::function<bool(const Pending&)>& valid)
{
    std::unique_lock lock(mutex_);

    // abandon() only latches, so it may have landed before this seat was ever asked.
    if(abandoned_)
        throw GameAbandoned{};

    if(replaced_)
        return std::nullopt;

    // A member whose grace period ran out before their turn came is replaced without being asked.
    if(graceEnds_ && Clock::now() >= *graceEnds_)
    {
        replaced_ = true;
        lock.unlock();
        notifyTable();
        return std::nullopt;
    }

    // A timer needs someone to make the move when it runs out.
    const bool timed = turnTimer_ && computer_.provider;

    prompt.id = promptIds_->fetch_add(1) + 1;
    if(timed)
        prompt.deadline = Clock::now() + *turnTimer_;
    open_ = prompt;
    answer_.reset();
    lastPrompt_ = prompt.id;
    const bool openedAway = graceEnds_.has_value();

    // Encoded with the lock released; send() takes it again to post, so nothing goes out after
    // abandon().
    lock.unlock();
    send(promptMessage(prompt, Clock::now()));
    if(openedAway)
        notifyTable();
    lock.lock();

    while(true)
    {
        const std::uint64_t seen = version_;
        waitUntil(changed_, lock, deadlineOf(prompt),
                  [&] { return answer_.has_value() || abandoned_ || replaced_ || version_ != seen; });

        if(abandoned_)
        {
            open_.reset();
            throw GameAbandoned{};
        }

        // Replaced from outside, by the reaper or by the member leaving: the computer takes the turn.
        if(replaced_)
        {
            open_.reset();
            answer_.reset();
            lock.unlock();
            send(promptClosedMessage(prompt.id, false));
            return std::nullopt;
        }

        // Taken and cleared at once, or the next prompt would return it without waiting.
        const std::optional<Pending> answer = std::exchange(answer_, std::nullopt);
        if(answer && valid(*answer))
        {
            const bool wasAway = graceEnds_.has_value();
            open_.reset();
            timedOut_ = 0;
            lock.unlock();
            send(promptClosedMessage(prompt.id, true));
            if(wasAway)
                notifyTable();
            return answer->value;
        }

        // Checked even when an answer woke the wait, or a stream of invalid ones would hold it off.
        const std::optional<Clock::time_point> end = deadlineOf(prompt);
        if(end && Clock::now() >= *end)
        {
            // An away member is replaced, and so is one whose turns keep running out.
            const bool away = graceEnds_.has_value();
            if(away || ++timedOut_ >= timeoutsBeforeReplacement)
                replaced_ = true;
            const bool nowReplaced = replaced_;
            open_.reset();
            lock.unlock();
            send(promptClosedMessage(prompt.id, false));
            if(nowReplaced)
                notifyTable();
            return std::nullopt;
        }

        if(!answer)
            continue;

        // An invalid answer never reaches the engine, which would end the game. The prompt stays
        // open with its deadline still running, so endless nonsense is answered by the computer.
        lock.unlock();
        send(errorMessage(ErrorCode::Illegal));
        send(promptMessage(prompt, Clock::now()));
        lock.lock();
    }
}

std::optional<Clock::time_point> SeatDriver::deadlineOf(const Prompt& prompt) const
{
    std::optional<Clock::time_point> end = prompt.deadline;
    if(graceEnds_ && (!end || *graceEnds_ < *end))
        end = graceEnds_;
    return end;
}

SeatDriver::Submitted SeatDriver::submit(const Answer& answer)
{
    {
        std::lock_guard lock(mutex_);

        // A replaced seat has no open prompt for its member, whatever is still on record.
        if(!open_ || replaced_)
            return answer.prompt != 0 && answer.prompt <= lastPrompt_ ? Submitted::Stale : Submitted::NotYourTurn;
        if(answer.prompt != open_->id || answer_)
            return Submitted::Stale;

        answer_ = Pending{answer.kind, answer.value};
    }
    changed_.notify_all();
    return Submitted::Taken;
}

void SeatDriver::abandon()
{
    {
        std::lock_guard lock(mutex_);
        abandoned_ = true;
    }
    changed_.notify_all();
}

void SeatDriver::away(Clock::time_point graceEnds)
{
    if(!member_ || !computer_.provider)
        return;
    {
        std::lock_guard lock(mutex_);
        graceEnds_ = graceEnds;
        version_++;
    }
    changed_.notify_all();
}

void SeatDriver::here()
{
    {
        std::lock_guard lock(mutex_);
        if(!graceEnds_)
            return;
        graceEnds_.reset();
        version_++;
    }
    changed_.notify_all();
}

void SeatDriver::replace(bool silent)
{
    if(!member_ || !computer_.provider)
        return;
    {
        std::lock_guard lock(mutex_);
        replaced_ = true;
        silent_ = silent_ || silent;
        version_++;
    }
    changed_.notify_all();
}

bool SeatDriver::reclaim()
{
    {
        std::lock_guard lock(mutex_);
        if(!replaced_)
            return false;
        replaced_ = false;
        silent_ = false;
        timedOut_ = 0;
        version_++;
    }
    changed_.notify_all();
    return true;
}

bool SeatDriver::replaced() const
{
    std::lock_guard lock(mutex_);
    return replaced_;
}

std::optional<Clock::time_point> SeatDriver::awayDeadline() const
{
    std::lock_guard lock(mutex_);
    if(!open_ || !graceEnds_)
        return std::nullopt;
    return deadlineOf(*open_);
}

void SeatDriver::withOpenPrompt(const std::function<void(const std::optional<Prompt>&)>& use) const
{
    std::lock_guard lock(mutex_);
    use(open_);
}

bool SeatDriver::humanInCharge() const
{
    std::lock_guard lock(mutex_);
    return member_ && !replaced_;
}

std::optional<MemberId> SeatDriver::member() const
{
    return member_;
}

IGameObserver* SeatDriver::observer() const
{
    return computer_.observer;
}

void SeatDriver::send(Json::Value message)
{
    // Checked under the lock abandon() takes, so an abandoned seat sends nothing more, nor one whose
    // member has left the game.
    std::lock_guard lock(mutex_);
    if(member_ && !abandoned_ && !silent_)
        outbox_.send(*member_, std::move(message));
}

void SeatDriver::notifyTable() const
{
    if(onChange_)
        onChange_();
}

} // namespace whist
