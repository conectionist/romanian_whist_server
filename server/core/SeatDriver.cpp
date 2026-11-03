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
                       std::shared_ptr<std::atomic<std::uint64_t>> promptIds)
    : seat_(seat),
      member_(member),
      computer_(std::move(computer)),
      turnTimer_(turnTimer),
      outbox_(outbox),
      promptIds_(std::move(promptIds))
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

    // A timer needs someone to make the move when it runs out.
    const bool timed = turnTimer_ && computer_.provider;

    prompt.id = promptIds_->fetch_add(1) + 1;
    if(timed)
        prompt.deadline = Clock::now() + *turnTimer_;
    open_ = prompt;
    answer_.reset();
    lastPrompt_ = prompt.id;

    // Sent with the lock released, so an answer is never held up while a prompt is encoded.
    lock.unlock();
    send(promptMessage(prompt, Clock::now()));
    lock.lock();

    while(true)
    {
        waitUntil(changed_, lock, prompt.deadline, [this] { return answer_.has_value() || abandoned_; });

        if(abandoned_)
        {
            open_.reset();
            throw GameAbandoned{};
        }

        // Taken and cleared at once, or the next prompt would return it without waiting.
        const std::optional<Pending> answer = std::exchange(answer_, std::nullopt);
        if(answer && valid(*answer))
        {
            open_.reset();
            lock.unlock();
            send(promptClosedMessage(prompt.id, true));
            return answer->value;
        }

        // Checked even when an answer woke the wait, or a stream of invalid ones would hold it off.
        if(prompt.deadline && Clock::now() >= *prompt.deadline)
        {
            open_.reset();
            lock.unlock();
            send(promptClosedMessage(prompt.id, false));
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

SeatDriver::Submitted SeatDriver::submit(const Answer& answer)
{
    {
        std::lock_guard lock(mutex_);
        if(!open_)
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

void SeatDriver::withOpenPrompt(const std::function<void(const std::optional<Prompt>&)>& use) const
{
    std::lock_guard lock(mutex_);
    use(open_);
}

bool SeatDriver::humanInCharge() const
{
    return member_.has_value();
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
    // Checked under the lock abandon() takes, so an abandoned seat sends nothing more.
    std::lock_guard lock(mutex_);
    if(member_ && !abandoned_)
        outbox_.send(*member_, std::move(message));
}

} // namespace whist
