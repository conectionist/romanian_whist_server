#ifndef WHIST_CORE_COMPUTERPLAYER_H
#define WHIST_CORE_COMPUTERPLAYER_H

#include "core/Settings.h"

#include <romanian_whist/AiMoveProvider.h>
#include <romanian_whist/IGameObserver.h>
#include <romanian_whist/Seat.h>

#include <cstdint>
#include <memory>
#include <optional>

namespace whist
{

struct ComputerPlayer
{
    std::unique_ptr<romanian_whist::AiMoveProvider> provider;

    // The strategy, when it learns the game by watching it. It must be registered with the engine
    // before start(), and outlive it.
    romanian_whist::IGameObserver* observer = nullptr;
};

// A Cyborg or a Reckoner is told its seat outright, so it can watch a seat it does not yet play.
ComputerPlayer makeComputerPlayer(Level level, romanian_whist::Seat seat, std::optional<std::uint32_t> seed);

} // namespace whist

#endif
