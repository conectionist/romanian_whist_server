#include "core/Pacing.h"

#include "core/Settings.h"

#include <algorithm>
#include <array>

namespace whist
{

Pacing pacingFor(int botSpeed, std::chrono::milliseconds floor)
{
    static constexpr std::array<double, 5> factors{2.0, 1.5, 1.0, 0.5, 0.0};
    const double factor = factors[static_cast<std::size_t>(std::clamp(botSpeed, slowestSpeed, fastestSpeed))];

    const Pacing defaults;
    const auto scale = [factor](std::chrono::milliseconds value)
    {
        return std::chrono::milliseconds(static_cast<long long>(static_cast<double>(value.count()) * factor));
    };

    Pacing pacing;
    pacing.beat = std::max(scale(defaults.beat), floor);
    pacing.spectatorBeat = std::max(scale(defaults.spectatorBeat), floor);
    pacing.trickDwell = scale(defaults.trickDwell);
    return pacing;
}

} // namespace whist
