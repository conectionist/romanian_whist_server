#ifndef WHIST_CORE_PACING_H
#define WHIST_CORE_PACING_H

#include <chrono>

namespace whist
{

// How long the game rests so people can follow it: a beat around a computer player's move (a
// shorter one when nobody at the table is human), and a dwell on a finished trick or round.
struct Pacing
{
    std::chrono::milliseconds beat{700};
    std::chrono::milliseconds spectatorBeat{250};
    std::chrono::milliseconds trickDwell{1200};
};

// The desktop client's five speeds, slow (0) to instant (4). The floor keeps a game of computer
// players from holding a core for its whole length at instant speed.
Pacing pacingFor(int botSpeed, std::chrono::milliseconds floor);

} // namespace whist

#endif
