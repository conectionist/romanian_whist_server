#include "core/Pacing.h"
#include "core/Settings.h"

#include "support/Games.h"

#include <catch2/catch_test_macros.hpp>

using namespace whist;
using std::chrono::milliseconds;

TEST_CASE("Settings the engine would refuse are refused first", "[settings]")
{
    const Settings good = test::tableSettings(3, {0});
    CHECK_FALSE(checkSettings(good, Mode::Solo));

    const auto fails = [](Settings settings, Mode mode, const std::string& field)
    {
        const auto error = checkSettings(settings, mode);
        REQUIRE(error);
        CHECK(error->field == field);
    };

    fails(test::tableSettings(1, {0}), Mode::Solo, "seats");
    Settings seven = test::tableSettings(6, {0});
    seven.seats.push_back(seven.seats.back());
    fails(seven, Mode::Solo, "seats");
    fails(test::tableSettings(3, {0, 1}), Mode::Solo, "seats");

    Settings open = good;
    open.seats[2] = SeatSetting{};
    fails(open, Mode::Solo, "seats");

    Settings clash = good;
    clash.seats[1].profile.name = "ANA";
    fails(clash, Mode::Solo, "name");

    for(const char* name : {"", " ", "TwentyOneCharactersXX", "Ana\tB", "Ana\xE2\x80\xAE", "Bad\xC3", "MrLowRisk",
                            "the ducking one"})
    {
        Settings bad = good;
        bad.seats[0].profile.name = name;
        INFO(name);
        fails(bad, Mode::Solo, "name");
    }

    Settings longName = good;
    longName.seats[0].profile.name = "Ștefănescu-Ionescu Ă";
    CHECK_FALSE(checkSettings(longName, Mode::Solo));

    Settings noLevel = good;
    noLevel.seats[1].level.reset();
    fails(noLevel, Mode::Solo, "level");

    Settings gender = good;
    gender.seats[0].profile.gender = "other";
    fails(gender, Mode::Solo, "gender");

    Settings avatar = good;
    avatar.seats[0].profile.avatar = "../x";
    fails(avatar, Mode::Solo, "avatar");

    Settings timer = good;
    timer.turnTimer = milliseconds(60000);
    fails(timer, Mode::Solo, "turnTimer");
    timer.replacement = Level::ReckonerBeta;
    fails(timer, Mode::Solo, "replacement");
    CHECK_FALSE(checkSettings(timer, Mode::Online));

    Settings timerAlone = good;
    timerAlone.turnTimer = milliseconds(60000);
    fails(timerAlone, Mode::Online, "turnTimer");

    Settings seeded = good;
    seeded.seed = 3;
    fails(seeded, Mode::Online, "seed");
}

TEST_CASE("Bot speed paces the game as the desktop client does", "[settings]")
{
    const Pacing normal = pacingFor(2, milliseconds(0));
    CHECK(normal.beat == milliseconds(700));
    CHECK(normal.spectatorBeat == milliseconds(250));
    CHECK(normal.trickDwell == milliseconds(1200));

    const Pacing slow = pacingFor(0, milliseconds(0));
    CHECK(slow.beat == milliseconds(1400));
    CHECK(slow.trickDwell == milliseconds(2400));

    const Pacing instant = pacingFor(4, milliseconds(30));
    CHECK(instant.beat == milliseconds(30));
    CHECK(instant.spectatorBeat == milliseconds(30));
    CHECK(instant.trickDwell == milliseconds(0));
}
