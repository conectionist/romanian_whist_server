#include "core/Publisher.h"

#include "core/Wait.h"

#include <romanian_whist/GameEngine.h>
#include <romanian_whist/Round.h>

#include <algorithm>
#include <utility>

namespace whist
{

using namespace romanian_whist;

namespace
{

std::vector<StandingRow> rows(const std::vector<Standing>& standings)
{
    std::vector<StandingRow> result;
    for(const Standing& standing : standings)
        result.push_back({standing.seat.index, standing.name, standing.score, standing.place});
    return result;
}

} // namespace

Publisher::Publisher(unsigned int game, std::vector<Viewer> viewers, Pacing pacing, bool waitAfterRound,
                     std::optional<std::chrono::milliseconds> holdLimit,
                     const std::vector<std::unique_ptr<SeatDriver>>& drivers, Outbox& outbox,
                     std::function<void()> changed)
    : game_(game),
      viewers_(std::move(viewers)),
      drivers_(drivers),
      outbox_(outbox),
      anyHuman_(std::ranges::any_of(drivers, [](const auto& driver) { return driver->member().has_value(); })),
      holdLimit_(holdLimit),
      changed_(std::move(changed)),
      latest_(drivers.size() + 1),
      pacing_(pacing),
      waitAfterRound_(waitAfterRound)
{
}

std::size_t Publisher::slotOf(std::optional<unsigned int> seat) const
{
    return seat ? *seat : drivers_.size();
}

void Publisher::publish(const GameEngine& engine, const GameEvent& event, std::optional<LastTrick> finishedTrick)
{
    // Built and encoded outside the lock; one snapshot per distinct viewpoint.
    std::vector<std::optional<GameSnapshot>> built(latest_.size());
    std::vector<std::optional<Json::Value>> encoded(latest_.size());
    for(const Viewer& viewer : viewers_)
    {
        const std::size_t slot = slotOf(viewer.seat);
        if(built[slot])
            continue;

        std::optional<Seat> seat;
        if(viewer.seat)
            seat = Seat{*viewer.seat};
        built[slot] = snapshotFrom(engine, seat);
        encoded[slot] = encodeSnapshot(*built[slot]);
    }

    std::lock_guard lock(cacheMutex_);
    for(std::size_t slot = 0 ; slot < built.size() ; slot++)
    {
        if(built[slot])
            latest_[slot] = std::move(built[slot]);
    }
    if(finishedTrick)
        lastTrick_ = std::move(finishedTrick);
    if(event.result)
        history_.push_back(*event.result);
    broadcastLocked([&](const Viewer& viewer) { return gameMessage(game_, event, *encoded[slotOf(viewer.seat)]); });
    if(event.kind == EventKind::GameOver)
        gameOverSent_ = true;
}

void Publisher::broadcastLocked(const std::function<Json::Value(const Viewer&)>& message)
{
    if(silenced_)
        return;
    for(const Viewer& viewer : viewers_)
    {
        if(std::ranges::find(muted_, viewer.member) == muted_.end())
            outbox_.send(viewer.member, message(viewer));
    }
}

bool Publisher::computerMoves(Seat seat) const
{
    return !drivers_.at(seat.index)->humanInCharge();
}

std::chrono::milliseconds Publisher::lengthOf(Rest kind) const
{
    if(kind == Rest::Dwell)
        return pacing_.trickDwell;
    return anyHuman_ ? pacing_.beat : pacing_.spectatorBeat;
}

// The desktop's pace(): a pause or a hold waits out the rest under way, then holds, and a rest that
// begins paused or held is the hold alone. A pause has no deadline, nor has a solo hold, nor has a
// game nobody is here to watch: the closing path cancels. An online hold ends at its deadline.
void Publisher::rest(Rest kind)
{
    std::unique_lock lock(restMutex_);
    if(!paused_ && !heldRound_ && !nobodyHere_)
    {
        const Clock::time_point started = Clock::now();
        while(!cancelled_)
        {
            // Measured again on every wake, so a new speed applies to this rest too.
            const Clock::time_point end = started + lengthOf(kind);
            if(Clock::now() >= end)
                break;

            // A wait with a deadline wakes early only when its condition holds, so a speed change
            // needs one of its own.
            const std::uint64_t seen = pacingVersion_;
            waitUntil(restChanged_, lock, end, [&] { return cancelled_ || pacingVersion_ != seen; });
        }
    }

    const auto free = [this] { return cancelled_ || (!paused_ && !heldRound_ && !nobodyHere_); };
    while(!waitUntil(restChanged_, lock, heldRound_ ? holdDeadline_ : std::nullopt, free))
    {
        // Only a held wait has a deadline, and its time is up.
        endHoldLocked();
    }
}

void Publisher::cancel()
{
    {
        std::lock_guard lock(restMutex_);
        cancelled_ = true;
    }
    restChanged_.notify_all();
}

void Publisher::silence()
{
    std::lock_guard lock(cacheMutex_);
    silenced_ = true;
}

void Publisher::setPaused(bool paused)
{
    {
        std::lock_guard lock(restMutex_);
        paused_ = paused;
    }
    restChanged_.notify_all();
}

void Publisher::setNobodyHere(bool nobodyHere)
{
    {
        std::lock_guard lock(restMutex_);
        nobodyHere_ = nobodyHere;
    }
    restChanged_.notify_all();
}

void Publisher::setMuted(MemberId member, bool muted)
{
    std::lock_guard lock(cacheMutex_);
    std::erase(muted_, member);
    if(muted)
        muted_.push_back(member);
}

void Publisher::continueRound(unsigned int round, std::optional<unsigned int> seat)
{
    std::unique_lock lock(restMutex_);
    if(heldRound_ != round)
        return;

    bool ended = false;
    if(holdLimit_)
        ended = seat && releaseLocked(*seat);
    else
    {
        endHoldLocked();
        ended = true;
    }
    lock.unlock();
    if(ended)
        restChanged_.notify_all();
}

void Publisher::excuse(unsigned int seat)
{
    std::unique_lock lock(restMutex_);
    if(!holdLimit_ || !heldRound_ || !releaseLocked(seat))
        return;
    lock.unlock();
    restChanged_.notify_all();
}

bool Publisher::releaseLocked(unsigned int seat)
{
    const auto found = std::ranges::find(awaited_, seat);
    if(found == awaited_.end())
        return false;
    awaited_.erase(found);
    if(awaited_.empty())
    {
        endHoldLocked();
        return true;
    }
    sendHoldLocked(*heldRound_, false);
    return false;
}

// Cleared from the cache too, so a sync taken before the game thread wakes shows no hold.
void Publisher::endHoldLocked()
{
    heldRound_.reset();
    holdAnnounced_ = false;
    awaited_.clear();
    holdDeadline_.reset();

    std::lock_guard cache(cacheMutex_);
    hold_.reset();
}

void Publisher::sendHoldLocked(unsigned int round, bool first)
{
    if(heldRound_ != round || (!first && !holdAnnounced_))
        return;
    holdAnnounced_ = true;

    HoldView view;
    view.round = round;
    view.deadline = holdDeadline_;
    if(holdLimit_)
        view.waitingFor = awaited_;
    else
    {
        for(unsigned int i = 0 ; i < drivers_.size() ; i++)
        {
            if(drivers_[i]->member())
                view.waitingFor.push_back(i);
        }
    }

    std::lock_guard cache(cacheMutex_);
    hold_ = view;
    const Clock::time_point now = Clock::now();
    broadcastLocked([&](const Viewer&) { return heldMessage(view, now); });
}

void Publisher::setPacing(Pacing pacing, bool waitAfterRound)
{
    {
        std::lock_guard lock(restMutex_);
        pacing_ = pacing;
        waitAfterRound_ = waitAfterRound;
        pacingVersion_++;
    }
    restChanged_.notify_all();
}

bool Publisher::gameOverSent() const
{
    std::lock_guard lock(cacheMutex_);
    return gameOverSent_;
}

void Publisher::onGameStarted(const GameEngine& engine)
{
    publish(engine, {EventKind::GameStarted});
}

void Publisher::onRoundStarted(const GameEngine& engine)
{
    publish(engine, {EventKind::RoundStarted});
}

// No beat here: the beat comes after a computer's bid, in onBetPlaced, and a human's own thinking
// is the pause before theirs.
void Publisher::onBetRequested(const GameEngine& engine, Seat seat)
{
    GameEvent event{EventKind::BetRequested};
    event.seat = seat.index;
    publish(engine, event);
}

void Publisher::onBetPlaced(const GameEngine& engine, Seat seat, unsigned int bet)
{
    GameEvent event{EventKind::BetPlaced};
    event.seat = seat.index;
    event.bet = bet;
    publish(engine, event);

    if(computerMoves(seat))
        rest(Rest::Beat);
}

// Without this beat the last bid is seen only when a computer made it.
void Publisher::onBettingComplete(const GameEngine& engine)
{
    publish(engine, {EventKind::BettingComplete});
    rest(Rest::Beat);
}

void Publisher::onTrickStarted(const GameEngine& engine, unsigned int trick, Seat leader)
{
    GameEvent event{EventKind::TrickStarted};
    event.trick = trick;
    event.seat = leader.index;
    publish(engine, event);
}

// The beat goes before a computer's card: onCardPlayed follows at once, and without it cards would
// seem to appear from nowhere.
void Publisher::onCardRequested(const GameEngine& engine, Seat seat)
{
    GameEvent event{EventKind::CardRequested};
    event.seat = seat.index;
    publish(engine, event);

    if(computerMoves(seat))
        rest(Rest::Beat);
}

void Publisher::onCardPlayed(const GameEngine& engine, Seat seat, const Card& card)
{
    GameEvent event{EventKind::CardPlayed};
    event.seat = seat.index;
    event.card = card;
    publish(engine, event);
}

void Publisher::onTrickWon(const GameEngine& engine, Seat winner, unsigned int trick)
{
    // The table still holds the finished trick here. Its cards are public, so any viewpoint will do.
    const GameSnapshot table = snapshotFrom(engine, std::nullopt);
    LastTrick last;
    last.round = table.roundIndex;
    last.ledBy = table.ledBy ? table.ledBy->index : winner.index;
    if(table.winning)
        last.winning = table.winning->index;
    last.cards = table.table;

    GameEvent event{EventKind::TrickWon};
    event.seat = winner.index;
    event.trick = trick;
    publish(engine, event, std::move(last));
    rest(Rest::Dwell);
}

void Publisher::onRoundScored(const GameEngine& engine)
{
    // The last callback at which the round index still names this round.
    const GameSnapshot scored = snapshotFrom(engine, std::nullopt);
    RoundResult result;
    result.index = scored.roundIndex;
    result.count = scored.roundCount;
    result.tricks = scored.trickCount;
    result.type = scored.roundType;
    result.trump = scored.trump;
    if(scored.roundIndex + 1 < scored.roundCount)
        result.nextCountsForStreaks = engine.getRound(scored.roundIndex + 1).getTrickCount() != 1;
    for(const SeatSnapshot& seat : scored.seats)
        result.seats.push_back({seat.bet, seat.tricksWon, seat.roundScore, seat.totalScore, seat.winStreak,
                                seat.lossStreak});

    const unsigned int round = result.index;
    GameEvent event{EventKind::RoundScored};
    event.result = std::move(result);

    // Read once, as the desktop does: changing the setting later leaves this round's hold alone.
    bool hold = false;
    {
        std::lock_guard lock(restMutex_);
        hold = waitAfterRound_ && !cancelled_;
        if(hold && holdLimit_)
        {
            // Online, nobody waits for a human who is not there.
            awaited_.clear();
            for(unsigned int seat = 0 ; seat < drivers_.size() ; seat++)
            {
                const std::optional<MemberId> member = drivers_[seat]->member();
                if(member && outbox_.isConnected(*member) && !drivers_[seat]->replaced())
                    awaited_.push_back(seat);
            }
            hold = !awaited_.empty();
            if(hold)
                holdDeadline_ = Clock::now() + *holdLimit_;
        }
        if(hold)
            heldRound_ = round;
    }

    publish(engine, event);
    if(hold)
    {
        std::lock_guard lock(restMutex_);
        sendHoldLocked(round, true);
    }

    rest(Rest::Dwell);

    // A hold cut short by closing ends here, so nothing of it follows roundComplete.
    std::lock_guard lock(restMutex_);
    if(heldRound_ == round)
        endHoldLocked();
}

void Publisher::onRoundComplete(const GameEngine& engine)
{
    publish(engine, {EventKind::RoundComplete});
}

void Publisher::onGameOver(const GameEngine& engine)
{
    GameEvent event{EventKind::GameOver};
    event.standings = rows(engine.getStandings());
    event.winners = rows(engine.getWinners());
    publish(engine, event);

    if(changed_)
        changed_();
}

void Publisher::onGameStopped(const GameEngine& engine)
{
    publish(engine, {EventKind::GameStopped});
}

void Publisher::announceFailure()
{
    std::lock_guard lock(cacheMutex_);
    broadcastLocked([&](const Viewer& viewer)
    {
        const std::optional<GameSnapshot>& snapshot = latest_[slotOf(viewer.seat)];
        return gameMessage(game_, {EventKind::GameStopped},
                           snapshot ? encodeSnapshot(*snapshot) : Json::Value(Json::nullValue));
    });
}

void Publisher::sendSync(MemberId member, TableView table, const std::optional<Prompt>& prompt)
{
    std::optional<unsigned int> seat;
    for(const Viewer& viewer : viewers_)
    {
        if(viewer.member == member)
            seat = viewer.seat;
    }

    SyncView sync;
    sync.table = std::move(table);
    sync.game = game_;
    sync.prompt = prompt;

    std::lock_guard lock(cacheMutex_);
    const std::optional<GameSnapshot>& snapshot = latest_[slotOf(seat)];
    if(snapshot)
        sync.snapshot = encodeSnapshot(*snapshot);
    sync.history = history_;
    sync.lastTrick = lastTrick_;
    sync.held = hold_;
    outbox_.send(member, syncMessage(sync, Clock::now()));
}

std::vector<RoundResult> Publisher::history() const
{
    std::lock_guard lock(cacheMutex_);
    return history_;
}

std::optional<LastTrick> Publisher::lastTrick() const
{
    std::lock_guard lock(cacheMutex_);
    return lastTrick_;
}

} // namespace whist
