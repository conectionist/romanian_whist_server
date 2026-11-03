#include "core/Cards.h"

#include <array>

namespace whist
{

using romanian_whist::Card;
using romanian_whist::Rank;
using romanian_whist::Suit;

namespace
{

constexpr std::string_view ranks = "23456789TJQKA";
constexpr std::string_view suits = "HDSC";

} // namespace

std::string cardCode(Card card)
{
    return {ranks[static_cast<std::size_t>(card.rank)], suits[static_cast<std::size_t>(card.suit)]};
}

std::string suitCode(Suit suit)
{
    return {suits[static_cast<std::size_t>(suit)]};
}

std::optional<Card> parseCard(std::string_view code)
{
    if(code.size() != 2)
        return std::nullopt;

    const std::size_t rank = ranks.find(code[0]);
    const std::size_t suit = suits.find(code[1]);
    if(rank == std::string_view::npos || suit == std::string_view::npos)
        return std::nullopt;

    return Card{static_cast<Rank>(rank), static_cast<Suit>(suit)};
}

} // namespace whist
