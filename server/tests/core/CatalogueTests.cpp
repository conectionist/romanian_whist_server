#include "core/Catalogue.h"
#include "core/Settings.h"

#include "support/Games.h"

#include <catch2/catch_test_macros.hpp>

#include <map>
#include <set>

using namespace whist;

namespace
{

std::size_t ordinaryNames(const Catalogue& catalogue, const std::string& gender, const std::string& language)
{
    std::size_t count = 0;
    for(const Catalogue::Name& name : catalogue.names())
    {
        if(name.strategy.empty() && (!name.gender || *name.gender == gender) &&
           (!name.language || *name.language == language))
            count++;
    }
    return count;
}

const Catalogue::Name* entryFor(const Catalogue& catalogue, const std::string& name)
{
    for(const Catalogue::Name& entry : catalogue.names())
    {
        if(entry.name == name)
            return &entry;
    }
    return nullptr;
}

const Catalogue::Avatar* avatarFor(const Catalogue& catalogue, const std::string& id)
{
    for(const Catalogue::Avatar& entry : catalogue.avatars())
    {
        if(entry.id == id)
            return &entry;
    }
    return nullptr;
}

} // namespace

TEST_CASE("The catalogues load, with enough names for a full table", "[catalogue]")
{
    const Catalogue& catalogue = Catalogue::get();
    CHECK(catalogue.names().size() == 75);
    CHECK(catalogue.avatars().size() == 17);
    for(const char* gender : {"female", "male"})
    {
        for(const char* language : {"en", "ro"})
            CHECK(ordinaryNames(catalogue, gender, language) >= 6);
    }
}

TEST_CASE("A catalogue the desktop client would refuse is refused", "[catalogue]")
{
    const std::string avatars = R"([{ "id": "a", "gender": "male", "file": "a.png" }])";
    CHECK_THROWS(Catalogue("{}", avatars));
    CHECK_THROWS(Catalogue(R"([{ "name": "", "gender": "male", "language": "en" }])", avatars));
    CHECK_THROWS(Catalogue(R"([{ "name": "Ana", "gender": "other", "language": "en" }])", avatars));
    CHECK_THROWS(Catalogue(R"([{ "name": "Ana", "gender": "female", "language": "fr" }])", avatars));
    CHECK_THROWS(Catalogue(R"([{ "name": "Ana", "gender": "female", "language": "en", "strategy": "chess" }])",
                           avatars));
    CHECK_THROWS(Catalogue(R"([{ "name": "Ana", "gender": "female", "language": "en" },
                               { "name": "ANA", "gender": "female", "language": "en" }])", avatars));
    CHECK_NOTHROW(Catalogue(R"([{ "name": "Ana", "gender": "female", "language": "en" },
                                { "name": "Ana", "gender": "female", "language": "ro" }])", avatars));

    const std::string names = R"([{ "name": "Ana", "gender": "female", "language": "en" }])";
    CHECK_THROWS(Catalogue(names, R"([{ "id": "Bad Id", "gender": "male", "file": "a.png" }])"));
    CHECK_THROWS(Catalogue(names, R"([{ "id": "a", "gender": "male" }])"));
    CHECK_THROWS(Catalogue(names, R"([{ "id": "a", "gender": "male", "file": "a.png" },
                                      { "id": "a", "gender": "female", "file": "b.png" }])"));
}

TEST_CASE("A person's portrait is for their gender and kept for no level", "[catalogue]")
{
    const Catalogue& catalogue = Catalogue::get();
    CHECK(catalogue.suitsHuman("woman-long-red-hair", "female"));
    CHECK_FALSE(catalogue.suitsHuman("woman-long-red-hair", "male"));
    CHECK_FALSE(catalogue.suitsHuman("cyborg-woman-eye", "female"));
    CHECK_FALSE(catalogue.suitsHuman("test-face", "female"));

    CHECK(catalogue.suitsComputer("cyborg-man-eye", "male", Level::Cyborg));
    CHECK(catalogue.suitsComputer("man-stubble", "male", Level::Cyborg));
    CHECK_FALSE(catalogue.suitsComputer("cyborg-man-eye", "male", Level::Random));
    CHECK_FALSE(catalogue.suitsComputer("cyborg-man-eye", "female", Level::Cyborg));
}

TEST_CASE("Settings refuse a portrait the catalogue does not give that seat", "[catalogue][settings]")
{
    const auto fails = [](const Settings& settings)
    {
        const auto error = checkSettings(settings, Mode::Solo);
        REQUIRE(error);
        CHECK(error->field == "avatar");
    };

    Settings settings = test::tableSettings(3, {0}, Level::Cyborg);
    REQUIRE_FALSE(checkSettings(settings, Mode::Solo));

    Settings unknown = settings;
    unknown.seats[1].profile.avatar = "test-face";
    fails(unknown);

    Settings human = settings;
    human.seats[0].profile.avatar = "cyborg-woman-eye";
    fails(human);

    Settings gender = settings;
    gender.seats[0].profile.avatar = "man-stubble";
    fails(gender);

    Settings cyborg = settings;
    cyborg.seats[1].profile.avatar = "cyborg-man-gold";
    CHECK_FALSE(checkSettings(cyborg, Mode::Solo));

    Settings random = test::tableSettings(3, {0}, Level::Random);
    random.seats[1].profile.avatar = "cyborg-man-gold";
    fails(random);
}

TEST_CASE("A computer player's name is its level's own first, then anyone's, and never taken", "[catalogue]")
{
    const Catalogue& catalogue = Catalogue::get();
    std::mt19937_64 random(11);

    for(int i = 0 ; i < 50 ; i++)
    {
        const Profile cyborg = catalogue.drawComputer(Level::Cyborg, "en", std::string("male"), 2, {}, {}, random);
        CHECK(cyborg.name == "Cyborg");
        CHECK(cyborg.gender == "male");
        CHECK(avatarFor(catalogue, cyborg.avatar)->strategy == "cyborg");

        const Profile duck = catalogue.drawComputer(Level::Ducking, "ro", std::string("female"), 2, {}, {}, random);
        CHECK(duck.name == "R\xC4\x83\xC8\x9Bu\xC8\x99" "ca");

        // Nobody's names are kept for the Reckoners, and the language is the setter's.
        const Profile reckoner = catalogue.drawComputer(Level::ReckonerBeta, "ro", std::nullopt, 2, {}, {}, random);
        const Catalogue::Name* entry = entryFor(catalogue, reckoner.name);
        REQUIRE(entry);
        CHECK(entry->strategy.empty());
        CHECK((!entry->language || *entry->language == "ro"));
        CHECK((!entry->gender || *entry->gender == reckoner.gender));
        CHECK(catalogue.suitsComputer(reckoner.avatar, reckoner.gender, Level::ReckonerBeta));
    }

    // With "Cyborg" taken, a second Cyborg gets anyone's name.
    const Profile second = catalogue.drawComputer(Level::Cyborg, "en", std::string("male"), 3,
                                                  {nameKey("CYBORG")}, {}, random);
    CHECK(second.name != "Cyborg");
    CHECK(entryFor(catalogue, second.name)->strategy.empty());
}

TEST_CASE("Drawn names never repeat, and run out into Bot N", "[catalogue]")
{
    const Catalogue& catalogue = Catalogue::get();
    std::mt19937_64 random(5);
    std::set<std::string> taken;

    const std::size_t available = ordinaryNames(catalogue, "female", "en");
    for(std::size_t i = 0 ; i < available ; i++)
    {
        const Profile profile = catalogue.drawComputer(Level::FirstCard, "en", std::string("female"), 4, taken, {},
                                                       random);
        CHECK(entryFor(catalogue, profile.name));
        CHECK(taken.insert(nameKey(profile.name)).second);
    }

    const Profile bot = catalogue.drawComputer(Level::FirstCard, "en", std::string("female"), 4, taken, {}, random);
    CHECK(bot.name == "Bot 4");
    taken.insert(nameKey(bot.name));
    CHECK(catalogue.drawComputer(Level::FirstCard, "en", std::string("female"), 4, taken, {}, random).name == "Bot 5");
}

TEST_CASE("Portraits go to those worn least, counting everyone's", "[catalogue]")
{
    const Catalogue& catalogue = Catalogue::get();
    std::mt19937_64 random(3);

    std::map<std::string, unsigned int> worn{{"woman-long-red-hair", 1}};
    std::set<std::string> seen;
    for(int i = 0 ; i < 4 ; i++)
    {
        const Profile profile = catalogue.drawComputer(Level::Random, "en", std::string("female"), 2, {}, worn, random);
        CHECK(profile.avatar != "woman-long-red-hair");
        CHECK(seen.insert(profile.avatar).second);
        worn[profile.avatar]++;
    }

    // Every woman's portrait is worn once, so the next may be any of them, the person's included.
    const Profile fifth = catalogue.drawComputer(Level::Random, "en", std::string("female"), 2, {}, worn, random);
    CHECK(catalogue.suitsHuman(fifth.avatar, "female"));
}

TEST_CASE("Only the deterministic computer players may be named for their kind", "[catalogue][settings]")
{
    Settings settings = test::tableSettings(3, {0}, Level::ReckonerGamma);
    settings.seats[1].profile.name = "Ducking Dan";
    const auto error = checkSettings(settings, Mode::Solo);
    REQUIRE(error);
    CHECK(error->field == "name");

    settings.seats[1].level = Level::Ducking;
    CHECK_FALSE(checkSettings(settings, Mode::Solo));
}
