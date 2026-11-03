#include "core/ComputerPlayer.h"

#include <romanian_whist/strategies/CyborgStrategy.h>
#include <romanian_whist/strategies/DuckingStrategy.h>
#include <romanian_whist/strategies/FirstCardStrategy.h>
#include <romanian_whist/strategies/LowRiskStrategy.h>
#include <romanian_whist/strategies/RandomCardStrategy.h>
#include <romanian_whist/strategies/ReckonerStrategy.h>

namespace whist
{

using namespace romanian_whist;

namespace
{

template<class Strategy>
ComputerPlayer watching(std::unique_ptr<Strategy> strategy, Seat seat)
{
    strategy->setSeat(seat);

    // Taken from the concrete type before the move: through IStrategy the pointer would not be
    // adjusted to the observer base.
    IGameObserver* observer = strategy.get();
    return ComputerPlayer{std::make_unique<AiMoveProvider>(std::move(strategy)), observer};
}

template<class Strategy>
ComputerPlayer plain(std::unique_ptr<Strategy> strategy)
{
    return ComputerPlayer{std::make_unique<AiMoveProvider>(std::move(strategy)), nullptr};
}

} // namespace

ComputerPlayer makeComputerPlayer(Level level, Seat seat, std::optional<std::uint32_t> seed)
{
    switch(level)
    {
        case Level::Random:
            return plain(seed ? std::make_unique<RandomCardStrategy>(*seed) : std::make_unique<RandomCardStrategy>());
        case Level::FirstCard:
            return plain(std::make_unique<FirstCardStrategy>());
        case Level::LowRisk:
            return plain(std::make_unique<LowRiskStrategy>());
        case Level::Ducking:
            return plain(std::make_unique<DuckingStrategy>());
        case Level::Cyborg:
            return watching(std::make_unique<CyborgStrategy>(), seat);
        case Level::ReckonerAlpha:
            return watching(std::make_unique<ReckonerStrategy>(reckoner::ReckonerPreset::Alpha, seed), seat);
        case Level::ReckonerBeta:
            return watching(std::make_unique<ReckonerStrategy>(reckoner::ReckonerPreset::Beta, seed), seat);
        case Level::ReckonerGamma:
            return watching(std::make_unique<ReckonerStrategy>(reckoner::ReckonerPreset::Gamma, seed), seat);
        case Level::ReckonerDelta:
            return watching(std::make_unique<ReckonerStrategy>(reckoner::ReckonerPreset::Delta, seed), seat);
    }
    return plain(std::make_unique<LowRiskStrategy>());
}

} // namespace whist
