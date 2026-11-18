#ifndef WHIST_TESTS_CORE_ORACLE_H
#define WHIST_TESTS_CORE_ORACLE_H

#include "core/Publisher.h"
#include "support/Recorder.h"

#include <romanian_whist/IGameObserver.h>

#include <string>
#include <vector>

namespace whist::test
{

// Knows, from the engine itself, which cards each viewer may have seen so far. Registered before
// every other observer, it writes into the log ahead of the messages each event causes. It reads
// hands only through the engine's own viewer-filtered snapshot.
class Oracle : public romanian_whist::IGameObserver
{
public:
    Oracle(Recorder& recorder, std::vector<Viewer> viewers);

    void onRoundStarted(const romanian_whist::GameEngine& engine) override;
    void onCardPlayed(const romanian_whist::GameEngine& engine, romanian_whist::Seat seat,
                      const romanian_whist::Card& card) override;

private:
    Recorder& recorder_;
    const std::vector<Viewer> viewers_;
};

// Every card anywhere in every message must be one its member could see at that moment: the
// hands they may see this round, the trump, and the cards played. Cards in a past round's record
// are checked against that round. Returns what leaked, or nothing.
std::vector<std::string> scanForLeaks(const std::vector<Entry>& entries);

} // namespace whist::test

#endif
