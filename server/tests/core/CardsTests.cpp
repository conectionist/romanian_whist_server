#include "core/Cards.h"

#include <catch2/catch_test_macros.hpp>

using namespace romanian_whist;
using whist::cardCode;
using whist::parseCard;

TEST_CASE("Cards are written rank then suit, with T for the ten", "[cards]")
{
    CHECK(cardCode(Card{Rank::Ten, Suit::Hearts}) == "TH");
    CHECK(cardCode(Card{Rank::Ace, Suit::Spades}) == "AS");
    CHECK(cardCode(Card{Rank::Three, Suit::Clubs}) == "3C");
    CHECK(cardCode(Card{Rank::Queen, Suit::Diamonds}) == "QD");
}

TEST_CASE("Every card reads back as itself", "[cards]")
{
    for(int rank = 0 ; rank <= static_cast<int>(Rank::Ace) ; rank++)
    {
        for(int suit = 0 ; suit <= static_cast<int>(Suit::Clubs) ; suit++)
        {
            const Card card{static_cast<Rank>(rank), static_cast<Suit>(suit)};
            const auto parsed = parseCard(cardCode(card));
            REQUIRE(parsed);
            CHECK(*parsed == card);
        }
    }
}

TEST_CASE("Anything else is not a card", "[cards]")
{
    for(const char* text : {"", "T", "10H", "1H", "TX", "th", "AHS", "ZH"})
        CHECK_FALSE(parseCard(text));
}
