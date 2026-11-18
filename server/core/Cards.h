#ifndef WHIST_CORE_CARDS_H
#define WHIST_CORE_CARDS_H

#include <romanian_whist/Card.h>

#include <optional>
#include <string>
#include <string_view>

namespace whist
{

// A card on the wire is two characters, rank then suit: "TH" is the ten of hearts. The engine's
// Card::toString() writes the ten as "10", so the protocol has its own spelling.
std::string cardCode(romanian_whist::Card card);
std::string suitCode(romanian_whist::Suit suit);
std::optional<romanian_whist::Card> parseCard(std::string_view code);

} // namespace whist

#endif
