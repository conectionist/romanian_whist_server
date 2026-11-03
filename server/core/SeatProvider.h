#ifndef WHIST_CORE_SEATPROVIDER_H
#define WHIST_CORE_SEATPROVIDER_H

#include "core/SeatDriver.h"

#include <romanian_whist/GameEngine.h>
#include <romanian_whist/IMoveProvider.h>

namespace whist
{

// What the engine holds for a seat. The engine destroys its providers on the game thread, so this
// owns nothing, and the GameRun keeps the driver, which an I/O thread may still be answering.
class SeatProvider : public romanian_whist::IMoveProvider
{
public:
    SeatProvider(SeatDriver& driver, const romanian_whist::GameEngine& engine, romanian_whist::Seat seat)
        : driver_(driver), engine_(engine), seat_(seat)
    {
    }

    unsigned int makeBet(const romanian_whist::BetContext& context) override
    {
        return driver_.makeBet(context, !engine_.canSeeHand(seat_, seat_));
    }

    std::optional<std::size_t> playCard(const romanian_whist::PlayContext& context) override
    {
        return driver_.playCard(context, !engine_.canSeeHand(seat_, seat_));
    }

private:
    SeatDriver& driver_;
    const romanian_whist::GameEngine& engine_;
    const romanian_whist::Seat seat_;
};

} // namespace whist

#endif
