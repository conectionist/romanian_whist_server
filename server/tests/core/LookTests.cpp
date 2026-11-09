#include "core/Catalogue.h"

#include "look/DesktopRules.h"

#include <catch2/catch_test_macros.hpp>

#include <stdexcept>

using namespace whist;
using namespace whist::look;
using romanian_whist::Card;
using romanian_whist::Rank;
using romanian_whist::Suit;

TEST_CASE("The desktop's player bids the lowest bid it is offered, and plays the first legal card as its hand shows them",
          "[look][desktop]")
{
    // The oracle is the desktop's own test of how a hand is laid out.
    const std::vector<Card> hand{Card(Rank::Seven, Suit::Diamonds), Card(Rank::Ace, Suit::Spades),
                                 Card(Rank::King, Suit::Hearts), Card(Rank::Queen, Suit::Spades)};
    const std::optional<Card> diamonds = Card(Rank::Five, Suit::Diamonds);

    using Positions = std::vector<std::size_t>;
    CHECK(desktop::displayOrder(hand, std::nullopt) == Positions{1, 3, 2, 0});
    CHECK(desktop::displayOrder(hand, diamonds) == Positions{0, 1, 3, 2});

    CHECK(desktop::firstShownLegal(hand, {0, 1, 2, 3}, std::nullopt) == 1);
    CHECK(desktop::firstShownLegal(hand, {1, 3}, std::nullopt) == 1);

    // The engine lists its legal cards in its own order, which is not the one the hand shows.
    CHECK(desktop::firstShownLegal(hand, {3, 1}, std::nullopt) == 1);
    CHECK(desktop::firstShownLegal(hand, {2}, std::nullopt) == 2);
    CHECK(desktop::firstShownLegal(hand, {0, 1, 2, 3}, diamonds) == 0);
    CHECK_THROWS_AS(desktop::firstShownLegal(hand, {}, std::nullopt), std::invalid_argument);

    CHECK(desktop::lowestBid(3, std::nullopt) == 0);
    CHECK(desktop::lowestBid(3, 0) == 1);
    CHECK(desktop::lowestBid(3, 1) == 0);
    CHECK(desktop::lowestBid(1, 0) == 1);
    CHECK_THROWS_AS(desktop::lowestBid(0, 0), std::invalid_argument);
}

TEST_CASE("Each seat wears a face the desktop would deal it", "[look][desktop]")
{
    const Catalogue& catalogue = Catalogue::get();

    // A Cyborg wears a cyborg face, and nobody else does.
    const std::vector<desktop::Wearer> wearers{{"male", ""},        {"male", "cyborg"}, {"female", "cyborg"},
                                               {"female", "ducking"}, {"male", "cyborg"}, {"female", ""}};
    const std::vector<std::string> faces = desktop::faces(wearers, catalogue.avatars(), 42);
    REQUIRE(faces.size() == wearers.size());
    CHECK(faces == desktop::faces(wearers, catalogue.avatars(), 42));

    for(std::size_t i = 0 ; i < faces.size() ; i++)
    {
        INFO("wearer " << i << " wears " << faces[i]);
        std::string strategy = "none";
        for(const Catalogue::Avatar& avatar : catalogue.avatars())
        {
            if(avatar.id == faces[i])
                strategy = avatar.strategy;
        }
        CHECK((strategy == "cyborg") == (wearers[i].strategy == "cyborg"));
    }
}
