#ifndef WHIST_LOOK_DESKTOPRULES_H
#define WHIST_LOOK_DESKTOPRULES_H

#include "core/Catalogue.h"

#include <romanian_whist/Card.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

// What the desktop client's screenshot tool does that the server's own code does not, so that the
// look fixtures show the same games: how its player answers, and which faces it deals.
namespace whist::look::desktop
{

// The lowest bid the bid strip offers: 0 to `max`, leaving out the forbidden one.
unsigned int lowestBid(unsigned int max, std::optional<unsigned int> forbidden);

// The positions of `hand` in the order the desktop shows it: the trump suit first, then spades,
// hearts, clubs and diamonds, the higher rank first within a suit.
std::vector<std::size_t> displayOrder(const std::vector<romanian_whist::Card>& hand,
                                      const std::optional<romanian_whist::Card>& trump);

// The first of `legal` (positions in `hand`, in the engine's order) in the order the desktop shows
// the hand, which is the card its player clicks. `legal` must not be empty.
std::size_t firstShownLegal(const std::vector<romanian_whist::Card>& hand, const std::vector<std::size_t>& legal,
                            const std::optional<romanian_whist::Card>& trump);

struct Wearer
{
    std::string gender;

    // The computer player's level, empty for a person.
    std::string strategy;
};

// A face for each wearer, by the desktop's rule: the strategy's own faces of the wearer's gender,
// else those kept for nobody. One of those worn least so far is drawn from a generator seeded with
// `seed`. An empty id means no face suits.
std::vector<std::string> faces(const std::vector<Wearer>& wearers, const std::vector<Catalogue::Avatar>& catalogue,
                               std::uint32_t seed);

} // namespace whist::look::desktop

#endif
