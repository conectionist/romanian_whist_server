#ifndef WHIST_TESTS_CORE_GAMES_H
#define WHIST_TESTS_CORE_GAMES_H

#include "core/GameRun.h"
#include "core/Settings.h"

#include <romanian_whist/Scoreboard.h>

#include <memory>
#include <optional>
#include <vector>

namespace whist::test
{

// Names that cannot be mistaken for cards by the leak scan.
inline const std::vector<std::string> names{"Ana", "Bogdan", "Cora", "Dan", "Ema", "Filip"};

// `seats` seats; those in `humans` are human, the rest computer players of `level`.
Settings tableSettings(unsigned int seats, const std::vector<unsigned int>& humans, Level level = Level::FirstCard);

// A game with no rests, so tests run as fast as the game can.
GameRunSetup quickGame(Settings settings, const std::vector<std::optional<MemberId>>& seatMembers,
                       std::optional<std::uint32_t> seed = 7);

} // namespace whist::test

#endif
