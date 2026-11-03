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

Publisher::Publisher(unsigned int game, std::vector<Viewer> viewers, Pacing pacing,
                     const std::vector<std::unique_ptr<SeatDriver>>& drivers, Outbox& outbox)
    : game_(game),
      viewers_(std::move(viewers)),
      pacing_(pacing),
      drivers_(drivers),
      outbox_(outbox),
      anyHuman_(std::ranges::any_of(drivers, [](const auto& driver) { return driver->humanInCharge(); })),
      latest_(drivers.size() + 1)
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
    for(const Viewer& viewer : viewers_)
        outbox_.send(viewer.member, gameMessage(game_, event, *encoded[slotOf(viewer.seat)]));
}

bool Publisher::computerMoves(Seat seat) const
{
    return !drivers_.at(seat.index)->humanInCharge();
}

void Publisher::rest(Rest kind)
{
    std::chrono::milliseconds length = pacing_.trickDwell;
    if(kind == Rest::Beat)
        length = anyHuman_ ? pacing_.beat : pacing_.spectatorBeat;

    if(length.count() <= 0)
        return;

    std::unique_lock lock(restMutex_);
    waitUntil(restChanged_, lock, Clock::now() + length, [this] { return cancelled_; });
}

void Publisher::cancel()
{
    {
        std::lock_guard lock(restMutex_);
        cancelled_ = true;
    }
    restChanged_.notify_all();
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

    GameEvent event{EventKind::RoundScored};
    event.result = std::move(result);
    publish(engine, event);
    rest(Rest::Dwell);
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
}

void Publisher::onGameStopped(const GameEngine& engine)
{
    publish(engine, {EventKind::GameStopped});
}

void Publisher::announceFailure()
{
    std::lock_guard lock(cacheMutex_);
    for(const Viewer& viewer : viewers_)
    {
        const std::optional<GameSnapshot>& snapshot = latest_[slotOf(viewer.seat)];
        outbox_.send(viewer.member, gameMessage(game_, {EventKind::GameStopped},
                                                snapshot ? encodeSnapshot(*snapshot) : Json::Value(Json::nullValue)));
    }
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
