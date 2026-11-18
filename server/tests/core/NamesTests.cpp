#include "core/Names.h"
#include "core/Settings.h"

#include "support/Games.h"

#include <catch2/catch_test_macros.hpp>

using namespace whist;

TEST_CASE("A name is trimmed, its spaces made plain and single, and composed", "[names]")
{
    CHECK(cleanName("  Ana   Maria  ") == "Ana Maria");
    CHECK(cleanName("Ana\xC2\xA0Maria") == "Ana Maria");
    CHECK(cleanName("Ana\xE3\x80\x80Maria") == "Ana Maria");

    // S with a combining comma below is the one letter Ș.
    CHECK(cleanName("S\xCC\xA6tefan") == "\xC8\x98tefan");

    // 23 code points as sent, 20 once composed.
    const std::string decomposed = "S\xCC\xA6tefa\xCC\x86nescu-Ionescu A\xCC\x86";
    CHECK(codePointCount(decomposed) == 23);
    const auto composed = cleanName(decomposed);
    REQUIRE(composed);
    CHECK(*composed == "\xC8\x98tef\xC4\x83nescu-Ionescu \xC4\x82");
    CHECK(codePointCount(*composed) == 20);
    CHECK(isCleanName(*composed));
    CHECK_FALSE(isCleanName(decomposed));
}

TEST_CASE("Invisible characters are dropped, and joiners kept for emoji", "[names]")
{
    CHECK(cleanName("An\xE2\x80\x8B" "a") == "Ana");
    CHECK(cleanName("An\xC2\xAD" "a") == "Ana");

    const std::string coder = "\xF0\x9F\x91\xA9\xE2\x80\x8D\xF0\x9F\x92\xBB";
    CHECK(cleanName(coder) == coder);
    const std::string heart = "Ana \xE2\x9D\xA4\xEF\xB8\x8F";
    CHECK(cleanName(heart) == heart);
}

TEST_CASE("A name that could hide or disguise itself is refused", "[names]")
{
    for(const char* name : {"", " ", "\xE3\x85\xA4", "\xE2\xA0\x80", "\xE2\x80\x8B", "Ana\tB", "Ana\n",
                            "Ana\xE2\x80\xAE", "\xE2\x81\xA6" "Ana", "\xD8\x9C" "Ana", "Ana\xEF\xBF\xB9" "x",
                            "TwentyOneCharactersXX", "Bad\xC3"})
    {
        INFO(name);
        CHECK_FALSE(cleanName(name));
    }
    CHECK(cleanName("TwentyCharactersXXXX"));
}

TEST_CASE("Names clash regardless of case, width, invisible characters and Romanian's two s's", "[names]")
{
    CHECK(nameKey("\xC8\x98TEFAN") == nameKey("\xC8\x99tefan"));
    CHECK(nameKey("\xC8\x98tefan") == nameKey("\xC5\x9E" "tefan"));
    CHECK(nameKey("\xC8\x9Aurcan") == nameKey("\xC5\xA2urcan"));
    CHECK(nameKey("\xEF\xBC\xA1\xEF\xBD\x8E\xEF\xBD\x81") == nameKey("Ana"));
    CHECK(nameKey("An\xE2\x80\x8D" "a") == nameKey("ana"));
    CHECK(nameKey("Ana") != nameKey("Anna"));
    CHECK(nameKey("Ana Maria") != nameKey("AnaMaria"));
}

TEST_CASE("A name already taken gets a number, within 20 characters", "[names]")
{
    std::set<std::string> taken;
    CHECK(numberedName("Ana", taken) == "Ana");

    taken.insert(nameKey("ana"));
    CHECK(numberedName("Ana", taken) == "Ana 2");
    taken.insert(nameKey("ANA 2"));
    CHECK(numberedName("Ana", taken) == "Ana 3");

    taken.insert(nameKey("Abcdefghijklmnopqrst"));
    CHECK(numberedName("Abcdefghijklmnopqrst", taken) == "Abcdefghijklmnopqr 2");

    // The cut ends on a space, which goes too.
    taken.insert(nameKey("Abcdefghijklmnopq rs"));
    CHECK(numberedName("Abcdefghijklmnopq rs", taken) == "Abcdefghijklmnopq 2");
}

TEST_CASE("A human may not be named for a deterministic computer player, in any form", "[names]")
{
    Settings settings = test::tableSettings(3, {0});
    for(const char* name : {"MrLowRisk", "the ducking one", "\xEF\xBC\xAC\xEF\xBD\x8F\xEF\xBD\x97Risk"})
    {
        INFO(name);
        settings.seats[0].profile.name = name;
        const auto error = checkSettings(settings, Mode::Solo);
        REQUIRE(error);
        CHECK(error->field == "name");
    }
}

TEST_CASE("Settings' names are cleaned before they are checked", "[names][settings]")
{
    Settings settings = test::tableSettings(3, {0});
    settings.seats[0].profile.name = "  Ana ";
    CHECK(checkSettings(settings, Mode::Solo));
    REQUIRE_FALSE(cleanNames(settings));
    CHECK(settings.seats[0].profile.name == "Ana");
    CHECK_FALSE(checkSettings(settings, Mode::Solo));

    settings.seats[1].profile.name = "\xE2\x80\x8B";
    const auto error = cleanNames(settings);
    REQUIRE(error);
    CHECK(error->field == "name");
}
