#ifndef WHIST_CORE_GAMEABANDONED_H
#define WHIST_CORE_GAMEABANDONED_H

#include <exception>

namespace whist
{

// How a seat driver parked on a human leaves run() when the table closes. A move provider has no
// other way out: every return value is a move, and an empty play means "no legal card", which the
// engine treats as a bug.
struct GameAbandoned : std::exception
{
    const char* what() const noexcept override
    {
        return "the game was abandoned while a seat was waiting for its human";
    }
};

} // namespace whist

#endif
