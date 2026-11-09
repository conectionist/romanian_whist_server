#include "core/Catalogue.h"

#include "look/DesktopRules.h"
#include "look/LookFixtures.h"

#include "support/Recorder.h"
#include "support/ServerMessages.h"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <stdexcept>

using namespace whist;
using namespace whist::look;
using romanian_whist::Card;
using romanian_whist::Rank;
using romanian_whist::Suit;

namespace
{

const std::chrono::milliseconds patience(60000 * test::slowdown);

const std::string regenerate = "Run ./build/tools/look/whist_look and commit the difference.";

std::optional<std::string> readFile(const std::filesystem::path& path)
{
    std::ifstream file(path, std::ios::binary);
    if(!file)
        return std::nullopt;
    std::stringstream contents;
    contents << file.rdbuf();
    return contents.str();
}

} // namespace

TEST_CASE("The look fixtures reach the desktop's moments, and are up to date", "[look]")
{
    const Built built = buildLookFiles(patience);

    for(const std::string& problem : built.problems)
        UNSCOPED_INFO(problem);
    REQUIRE(built.problems.empty());

    std::set<std::string> written;
    for(const LookFile& file : built.files)
    {
        INFO(file.name);
        written.insert(file.name);

        // A client draws a state as it draws a sync, so it has to read as one.
        const Json::Value& state = file.content["state"];
        if(!state.isNull())
        {
            Json::Value sync = state;
            sync["type"] = "sync";
            sync["seq"] = 1;
            CHECK(test::shapeError(sync).empty());
        }

        const std::filesystem::path path = lookDirectory() / file.name;
        const std::optional<std::string> committed = readFile(path);
        if(!committed)
            FAIL_CHECK(path.string() << " is missing. " << regenerate);
        else if(*committed != lookText(file.content))
            FAIL_CHECK(path.string() << " is not what the tool writes now. " << regenerate);
    }

    std::error_code none;
    for(const std::filesystem::directory_entry& entry : std::filesystem::directory_iterator(lookDirectory(), none))
    {
        if(entry.path().extension() == ".json" && !written.contains(entry.path().filename().string()))
            FAIL_CHECK(entry.path().string() << " is not written by the tool. " << regenerate);
    }
}

TEST_CASE("The look games play the same way twice", "[look]")
{
    const Built first = buildLookFiles(patience);
    const Built second = buildLookFiles(patience);

    REQUIRE(first.problems.empty());
    REQUIRE(second.problems.empty());
    REQUIRE(first.files.size() == second.files.size());
    for(std::size_t i = 0 ; i < first.files.size() ; i++)
    {
        INFO(first.files[i].name);
        CHECK(first.files[i].name == second.files[i].name);
        CHECK(lookText(first.files[i].content) == lookText(second.files[i].content));
    }
}

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

    for(unsigned int players = 2 ; players <= 6 ; players++)
    {
        INFO(players << " players");
        const Settings settings = shotSettings(players);
        REQUIRE(settings.seats.size() == players);

        std::set<std::string> worn;
        for(std::size_t seat = 0 ; seat < settings.seats.size() ; seat++)
        {
            INFO("seat " << seat);
            const SeatSetting& setting = settings.seats[seat];
            if(seat == 0)
                CHECK(catalogue.suitsHuman(setting.profile.avatar, setting.profile.gender));
            else
                CHECK(catalogue.suitsComputer(setting.profile.avatar, setting.profile.gender, *setting.level));
            worn.insert(setting.profile.avatar);
        }

        // The catalogue has faces enough for every seat to wear its own.
        CHECK(worn.size() == players);
        CHECK(shotSettings(players) == settings);
    }

    // A smaller table's faces are the first ones of a bigger table's: each is drawn in turn.
    const Settings six = shotSettings(6);
    const Settings three = shotSettings(3);
    for(std::size_t seat = 0 ; seat < three.seats.size() ; seat++)
        CHECK(three.seats[seat].profile.avatar == six.seats[seat].profile.avatar);

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
