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
    Publisher(unsigned int game, std::vector<Viewer> viewers, Pacing pacing,
              const std::vector<std::unique_ptr<SeatDriver>>& drivers, Outbox& outbox);

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

    // Any thread. Ends any rest at once and keeps later ones from starting. Rests return rather
    // than throw, so the game reaches its next boundary and stops there cleanly.
    void cancel();

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

    const unsigned int game_;
    const std::vector<Viewer> viewers_;
    const Pacing pacing_;
    const std::vector<std::unique_ptr<SeatDriver>>& drivers_;
    Outbox& outbox_;
    const bool anyHuman_;

    // Guards everything below. The game's messages are posted under it too (Outbox::send only
    // queues), which is what keeps a sync in order with them.
    mutable std::mutex cacheMutex_;
    std::vector<std::optional<romanian_whist::GameSnapshot>> latest_;
    std::vector<RoundResult> history_;
    std::optional<LastTrick> lastTrick_;

    std::mutex restMutex_;
    std::condition_variable restChanged_;
    bool cancelled_ = false;
};

} // namespace whist

#endif
