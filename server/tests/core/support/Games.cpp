#include "support/Games.h"

#include <algorithm>

namespace whist::test
{

Settings tableSettings(unsigned int seats, const std::vector<unsigned int>& humans, Level level)
{
    Settings settings;
    for(unsigned int i = 0 ; i < seats ; i++)
    {
        SeatSetting seat;
        const bool human = std::ranges::find(humans, i) != humans.end();
        seat.kind = human ? SeatKind::Human : SeatKind::Computer;
        if(!human)
            seat.level = level;
        seat.profile = i % 2 == 0 ? Profile{names[i], "female", "woman-long-red-hair"}
                                  : Profile{names[i], "male", "man-dark-hair-beard"};
        settings.seats.push_back(seat);
    }
    settings.botSpeed = fastestSpeed;
    return settings;
}

GameRunSetup quickGame(Settings settings, const std::vector<std::optional<MemberId>>& seatMembers,
                       std::optional<std::uint32_t> seed)
{
    GameRunSetup setup;
    setup.settings = std::move(settings);
    setup.seatMembers = seatMembers;
    setup.seatMembers.resize(setup.settings.seats.size());
    setup.shuffleSeed = seed;
    setup.instantFloor = std::chrono::milliseconds(0);
    setup.threadNiceness = 0;
    setup.promptIds = std::make_shared<std::atomic<std::uint64_t>>(0);
    return setup;
}

} // namespace whist::test
