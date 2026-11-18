#include "look/DesktopRules.h"

#include <algorithm>
#include <map>
#include <random>
#include <stdexcept>

namespace whist::look::desktop
{

using romanian_whist::Card;
using romanian_whist::Suit;

namespace
{

constexpr Suit suitOrder[] = {Suit::Spades, Suit::Hearts, Suit::Clubs, Suit::Diamonds};

unsigned int suitPosition(Suit suit, const std::optional<Card>& trump)
{
    if(trump && suit == trump->suit)
        return 0;
    for(unsigned int i = 0 ; i < 4 ; i++)
    {
        if(suitOrder[i] == suit)
            return i + 1;
    }
    return 5;
}

bool inDisplayOrder(const Card& left, const Card& right, const std::optional<Card>& trump)
{
    const unsigned int leftSuit = suitPosition(left.suit, trump);
    const unsigned int rightSuit = suitPosition(right.suit, trump);
    if(leftSuit != rightSuit)
        return leftSuit < rightSuit;
    return static_cast<int>(left.rank) > static_cast<int>(right.rank);
}

} // namespace

unsigned int lowestBid(unsigned int max, std::optional<unsigned int> forbidden)
{
    for(unsigned int bet = 0 ; bet <= max ; bet++)
    {
        if(bet != forbidden)
            return bet;
    }
    throw std::invalid_argument("no bid is offered");
}

std::vector<std::size_t> displayOrder(const std::vector<Card>& hand, const std::optional<Card>& trump)
{
    std::vector<std::size_t> order(hand.size());
    for(std::size_t i = 0 ; i < hand.size() ; i++)
        order[i] = i;

    // Positions, not cards, because the answer to the server is a position.
    std::sort(order.begin(), order.end(), [&hand, &trump](std::size_t left, std::size_t right)
    {
        return inDisplayOrder(hand[left], hand[right], trump);
    });
    return order;
}

std::size_t firstShownLegal(const std::vector<Card>& hand, const std::vector<std::size_t>& legal,
                            const std::optional<Card>& trump)
{
    for(const std::size_t position : displayOrder(hand, trump))
    {
        if(std::find(legal.begin(), legal.end(), position) != legal.end())
            return position;
    }
    throw std::invalid_argument("no card can be played");
}

std::vector<std::string> faces(const std::vector<Wearer>& wearers, const std::vector<Catalogue::Avatar>& catalogue,
                               std::uint32_t seed)
{
    std::mt19937 random(seed);

    // The faces of one strategy (empty: of none) that suit a gender, in catalogue order.
    const auto group = [&catalogue](const std::string& strategy, const std::string& gender)
    {
        std::vector<std::string> ids;
        for(const Catalogue::Avatar& avatar : catalogue)
        {
            if(avatar.strategy == strategy && (!avatar.gender || *avatar.gender == gender))
                ids.push_back(avatar.id);
        }
        return ids;
    };

    // A count that is never cleared: a face that suits both genders is in both groups, and one
    // group running out must not free a face the other group's wearer has.
    std::map<std::string, unsigned int> worn;
    const auto wornCount = [&worn](const std::string& id)
    {
        const auto found = worn.find(id);
        return found == worn.end() ? 0U : found->second;
    };

    std::vector<std::string> chosen;
    chosen.reserve(wearers.size());

    for(const Wearer& wearer : wearers)
    {
        std::vector<std::string> theirs = wearer.strategy.empty() ? std::vector<std::string>()
                                                                  : group(wearer.strategy, wearer.gender);
        if(theirs.empty())
            theirs = group(std::string(), wearer.gender);

        if(theirs.empty())
        {
            chosen.emplace_back();
            continue;
        }

        unsigned int least = wornCount(theirs.front());
        for(const std::string& id : theirs)
            least = std::min(least, wornCount(id));

        std::vector<std::string> free;
        for(const std::string& id : theirs)
        {
            if(wornCount(id) == least)
                free.push_back(id);
        }

        // Drawn even from one candidate, as the desktop does, so every later draw lines up.
        std::uniform_int_distribution<std::size_t> pick(0, free.size() - 1);
        const std::string& id = free[pick(random)];
        chosen.push_back(id);
        ++worn[id];
    }
    return chosen;
}

} // namespace whist::look::desktop
