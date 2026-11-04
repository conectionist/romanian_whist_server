#ifndef WHIST_CORE_PUBLISHER_H
#define WHIST_CORE_PUBLISHER_H

#include "core/Codec.h"
#include "core/Outbox.h"
#include "core/Pacing.h"
#include "core/Protocol.h"
#include "core/SeatDriver.h"

#include <romanian_whist/IGameObserver.h>
#include <romanian_whist/Snapshot.h>

#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <vector>

namespace whist
{

// Someone who sees the game: a seated member, or a watcher with no seat.
struct Viewer
{
    MemberId member = 0;
    std::optional<unsigned int> seat;
};

// The engine's observer for one game. It sends every viewer their own snapshot after each event,
// paces the game, and keeps what a viewer who comes back needs, so a sync never reads the engine.
class Publisher : public romanian_whist::IGameObserver
{
public:
    // With a hold limit, a game is online: its hold after a round waits only for the humans connected
    // when it starts, and for no longer than the limit. Without one, as in solo, it waits for as long
    // as it takes. `changed` is called on the game thread, with no lock held, once the game is over.
    Publisher(unsigned int game, std::vector<Viewer> viewers, Pacing pacing, bool waitAfterRound,
              std::optional<std::chrono::milliseconds> holdLimit,
              const std::vector<std::unique_ptr<SeatDriver>>& drivers, Outbox& outbox,
              std::function<void()> changed = {});

    void onGameStarted(const romanian_whist::GameEngine& engine) override;
    void onRoundStarted(const romanian_whist::GameEngine& engine) override;
    void onBetRequested(const romanian_whist::GameEngine& engine, romanian_whist::Seat seat) override;
    void onBetPlaced(const romanian_whist::GameEngine& engine, romanian_whist::Seat seat, unsigned int bet) override;
    void onBettingComplete(const romanian_whist::GameEngine& engine) override;
    void onTrickStarted(const romanian_whist::GameEngine& engine, unsigned int trick, romanian_whist::Seat leader) override;
    void onCardRequested(const romanian_whist::GameEngine& engine, romanian_whist::Seat seat) override;
    void onCardPlayed(const romanian_whist::GameEngine& engine, romanian_whist::Seat seat,
                      const romanian_whist::Card& card) override;
    void onTrickWon(const romanian_whist::GameEngine& engine, romanian_whist::Seat winner, unsigned int trick) override;
    void onRoundScored(const romanian_whist::GameEngine& engine) override;
    void onRoundComplete(const romanian_whist::GameEngine& engine) override;
    void onGameOver(const romanian_whist::GameEngine& engine) override;
    void onGameStopped(const romanian_whist::GameEngine& engine) override;

    // Any thread. Ends any rest or hold at once and keeps later ones from starting. Rests return
    // rather than throw, so the game reaches its next boundary and stops there cleanly.
    void cancel();

    // Any thread. Nothing is sent after it returns, not even the game's own gameStopped.
    void silence();

    // Any thread. A pause lands at the game's next rest and holds it there until resumed.
    void setPaused(bool paused);

    // Any thread. A Continue for `round` from a seat, or from a watcher (no seat). Solo, it ends that
    // round's hold. Online, it takes the seat off the hold, which ends once nobody is left on it.
    // Anything else changes nothing.
    void continueRound(unsigned int round, std::optional<unsigned int> seat);

    // Any thread. Online: the seat's human has gone, so the hold no longer waits for them.
    void excuse(unsigned int seat);

    // Any thread. While nobody who sees the game is connected, it holds at its next rest, as for a
    // pause, and goes on once someone is.
    void setNobodyHere(bool nobodyHere);

    // Any thread. A muted member is sent nothing of the game; once this returns nothing more is.
    void setMuted(MemberId member, bool muted);

    // Any thread. A new speed applies to the rest under way. The wait applies from the next scored
    // round, so a hold already on stays on.
    void setPacing(Pacing pacing, bool waitAfterRound);

    // Any thread. True once gameOver has been sent: nothing else of this game's follows it. Read
    // under the lock it is sent under, so it never reads false after gameOver has gone out.
    bool gameOverSent() const;

    // Game thread, after run() failed: tells every viewer the game stopped, from the cache.
    void announceFailure();

    // Any thread. Sends `member` a sync from the cache, under the lock the game's own messages are
    // sent under, so it is never overtaken by an older one or overtakes a newer one.
    void sendSync(MemberId member, TableView table, const std::optional<Prompt>& prompt);

    std::vector<RoundResult> history() const;
    std::optional<LastTrick> lastTrick() const;

private:
    enum class Rest
    {
        Beat,
        Dwell
    };

    // The viewer slot a seat's (or a watcher's) snapshot is kept in.
    std::size_t slotOf(std::optional<unsigned int> seat) const;

    // A finished trick is cached with the snapshots that show it, so a sync never holds one
    // without the other.
    void publish(const romanian_whist::GameEngine& engine, const GameEvent& event,
                 std::optional<LastTrick> finishedTrick = std::nullopt);
    void rest(Rest kind);
    bool computerMoves(romanian_whist::Seat seat) const;

    // With restMutex_ held. Sends the hold as it stands, if it is still on and has been announced,
    // or announces it when `first`.
    void sendHoldLocked(unsigned int round, bool first);

    // With restMutex_ held: takes the seat off an online hold, and ends the hold once nobody is left.
    // True when the hold ended.
    bool releaseLocked(unsigned int seat);
    void endHoldLocked();

    std::chrono::milliseconds lengthOf(Rest kind) const;

    // Sends to every viewer. Called with cacheMutex_ held.
    void broadcastLocked(const std::function<Json::Value(const Viewer&)>& message);

    const unsigned int game_;
    const std::vector<Viewer> viewers_;
    const std::vector<std::unique_ptr<SeatDriver>>& drivers_;
    Outbox& outbox_;
    const bool anyHuman_;
    const std::optional<std::chrono::milliseconds> holdLimit_;
    const std::function<void()> changed_;

    // Guards everything below. The game's messages are posted under it too (Outbox::send only
    // queues), which is what keeps a sync in order with them.
    mutable std::mutex cacheMutex_;
    std::vector<std::optional<romanian_whist::GameSnapshot>> latest_;
    std::vector<RoundResult> history_;
    std::optional<LastTrick> lastTrick_;
    std::optional<HoldView> hold_;
    bool silenced_ = false;
    std::vector<MemberId> muted_;

    bool gameOverSent_ = false;

    // Guards everything below. It may be held while cacheMutex_ is taken, to send the hold, and
    // never the other way round.
    std::mutex restMutex_;
    std::condition_variable restChanged_;
    Pacing pacing_;
    std::uint64_t pacingVersion_ = 0;
    bool waitAfterRound_;
    bool paused_ = false;
    bool nobodyHere_ = false;
    bool cancelled_ = false;

    // The hold after a scored round. It is set up before the round's result goes out, so a Continue
    // sent on seeing the result always counts, and announced just after it.
    std::optional<unsigned int> heldRound_;
    bool holdAnnounced_ = false;
    std::vector<unsigned int> awaited_;
    std::optional<Clock::time_point> holdDeadline_;
};

} // namespace whist

#endif
