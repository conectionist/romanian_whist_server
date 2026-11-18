#include "core/Catalogue.h"
#include "core/Seating.h"

#include <catch2/catch_test_macros.hpp>

#include <random>

using namespace whist;

namespace
{

constexpr MemberId host = 1;
constexpr MemberId bea = 2;
constexpr MemberId cora = 3;

Profile person(std::string name, std::string gender = "female")
{
    return {std::move(name), gender, gender == "female" ? "woman-long-red-hair" : "man-dark-hair-beard"};
}

SeatSetting computer(Level level, std::string name)
{
    SeatSetting seat;
    seat.kind = SeatKind::Computer;
    seat.level = level;
    seat.profile = {std::move(name), "male", "man-stubble"};
    return seat;
}

// Four seats, the last a Cyborg, with the host in seat 0.
Seating lobby()
{
    Settings settings;
    settings.seats.resize(4);
    settings.seats[3] = computer(Level::Cyborg, "Cyborg");
    completeOnline(settings);
    Seating seating(settings, {}, host);
    REQUIRE_FALSE(seating.join(host, person("Ana")));
    return seating;
}

// Each person's profile is in their seat and nowhere else, and every human seat has its person.
void checkOneOwner(const Seating& seating)
{
    const auto& seats = seating.settings().seats;
    for(unsigned int i = 0 ; i < seats.size() ; i++)
        CHECK((seats[i].kind == SeatKind::Human) == seating.memberAt(i).has_value());
    for(const TableMember& member : seating.members())
    {
        REQUIRE(member.seat);
        CHECK(seats.at(*member.seat).kind == SeatKind::Human);
    }
}

} // namespace

TEST_CASE("Newcomers take the first open seat, numbered when their name is taken", "[seating]")
{
    Seating seating = lobby();
    CHECK(seating.seatOf(host) == 0u);

    REQUIRE_FALSE(seating.join(bea, person("ANA")));
    CHECK(seating.seatOf(bea) == 1u);
    CHECK(seating.settings().seats[1].profile.name == "ANA 2");

    REQUIRE_FALSE(seating.join(cora, person("cyborg", "male")));
    CHECK(seating.settings().seats[2].profile.name == "cyborg 2");

    CHECK(seating.join(9, person("Dana")) == ErrorCode::TableFull);
    CHECK_FALSE(seating.hasMember(9));
    checkOneOwner(seating);
}

TEST_CASE("A host who leaves hands over to whoever has been there longest", "[seating]")
{
    Seating seating = lobby();
    REQUIRE_FALSE(seating.join(bea, person("Bea")));
    REQUIRE_FALSE(seating.join(cora, person("Cora")));

    seating.leave(host);
    CHECK(seating.host() == bea);
    CHECK(seating.settings().seats[0].kind == SeatKind::Open);

    seating.leave(cora);
    CHECK(seating.host() == bea);
    seating.leave(bea);
    CHECK(seating.members().empty());
    checkOneOwner(seating);
}

TEST_CASE("People move to open seats only, with their profile", "[seating]")
{
    Seating seating = lobby();
    REQUIRE_FALSE(seating.join(bea, person("Bea")));

    CHECK(seating.takeSeat(bea, 0) == ErrorCode::Illegal);
    CHECK(seating.takeSeat(bea, 3) == ErrorCode::Illegal);
    const auto past = seating.takeSeat(bea, 4);
    REQUIRE(past);
    CHECK(past->code == ErrorCode::BadMessage);
    CHECK(past->detail == "seat");

    REQUIRE_FALSE(seating.takeSeat(bea, 2));
    CHECK(seating.seatOf(bea) == 2u);
    CHECK(seating.settings().seats[2].profile.name == "Bea");
    CHECK(seating.settings().seats[1].kind == SeatKind::Open);
    checkOneOwner(seating);
}

TEST_CASE("The host sets seats open or to computer players, drawn from the catalogue", "[seating]")
{
    Seating seating = lobby();
    REQUIRE_FALSE(seating.join(bea, person("Bea")));
    std::mt19937_64 random(1);

    CHECK(seating.setSeat(bea, 2, SeatKind::Computer, Level::Random, "en", random) == ErrorCode::NotHost);
    CHECK(seating.setSeat(host, 1, SeatKind::Computer, Level::Random, "en", random) == ErrorCode::Illegal);
    CHECK(seating.setSeat(host, 4, SeatKind::Computer, Level::Random, "en", random) == ErrorCode::BadMessage);

    REQUIRE_FALSE(seating.setSeat(host, 2, SeatKind::Computer, Level::ReckonerBeta, "ro", random));
    const SeatSetting drawn = seating.settings().seats[2];
    CHECK(drawn.kind == SeatKind::Computer);
    CHECK(drawn.level == Level::ReckonerBeta);
    CHECK_FALSE(checkComputerProfile(drawn.profile, Level::ReckonerBeta));
    CHECK(nameKey(drawn.profile.name) != nameKey("Ana"));
    CHECK(nameKey(drawn.profile.name) != nameKey("Bea"));
    CHECK(nameKey(drawn.profile.name) != nameKey("Cyborg"));

    // The same level again changes nothing; another keeps the gender.
    REQUIRE_FALSE(seating.setSeat(host, 2, SeatKind::Computer, Level::ReckonerBeta, "ro", random));
    CHECK(seating.settings().seats[2] == drawn);
    REQUIRE_FALSE(seating.setSeat(host, 2, SeatKind::Computer, Level::Cyborg, "en", random));
    CHECK(seating.settings().seats[2].profile.gender == drawn.profile.gender);
    CHECK(seating.settings().seats[2].level == Level::Cyborg);

    REQUIRE_FALSE(seating.setSeat(host, 2, SeatKind::Open, std::nullopt, "en", random));
    CHECK(seating.settings().seats[2] == SeatSetting{});
    checkOneOwner(seating);
}

TEST_CASE("A profile may not take a name someone else has", "[seating]")
{
    Seating seating = lobby();
    REQUIRE_FALSE(seating.join(bea, person("Bea")));

    CHECK(seating.updateProfile(bea, person("ana")) == ErrorCode::NameTaken);
    CHECK(seating.updateProfile(bea, person("CYBORG")) == ErrorCode::NameTaken);
    REQUIRE_FALSE(seating.updateProfile(bea, person("BEA")));
    CHECK(seating.settings().seats[1].profile.name == "BEA");
    REQUIRE_FALSE(seating.updateProfile(bea, person("Bogdan", "male")));
    CHECK(seating.settings().seats[1].profile.gender == "male");
}

TEST_CASE("The host's settings change everything but what the seats hold", "[seating]")
{
    Seating seating = lobby();
    REQUIRE_FALSE(seating.join(bea, person("Bea")));

    Settings wanted = seating.settings();
    wanted.structure = romanian_whist::GameStructure::S_818;
    wanted.botSpeed = 4;
    wanted.waitAfterRound = true;
    wanted.replacement.reset();
    wanted.seats = {SeatSetting{}, SeatSetting{}, SeatSetting{}, SeatSetting{}, SeatSetting{}, SeatSetting{}};
    CHECK(seating.updateSettings(bea, wanted) == ErrorCode::NotHost);

    REQUIRE_FALSE(seating.updateSettings(host, wanted));
    const Settings& now = seating.settings();
    CHECK(now.structure == romanian_whist::GameStructure::S_818);
    CHECK(now.botSpeed == 4);
    CHECK(now.replacement == Level::ReckonerBeta);
    REQUIRE(now.seats.size() == 6);
    CHECK(now.seats[0].profile.name == "Ana");
    CHECK(now.seats[3].level == Level::Cyborg);
    CHECK(now.seats[5].kind == SeatKind::Open);

    // Seats off the end may go while they hold nobody.
    wanted.seats.resize(4);
    REQUIRE_FALSE(seating.updateSettings(host, wanted));
    wanted.seats.resize(1);
    CHECK(seating.updateSettings(host, wanted) == ErrorCode::BadMessage);
    REQUIRE_FALSE(seating.takeSeat(bea, 2));
    wanted.seats.resize(2);
    const auto cut = seating.updateSettings(host, wanted);
    REQUIRE(cut);
    CHECK(cut->code == ErrorCode::Illegal);
    CHECK(cut->detail == "seats");
    REQUIRE_FALSE(seating.takeSeat(bea, 1));
    REQUIRE_FALSE(seating.updateSettings(host, wanted));
    CHECK(seating.settings().seats.size() == 2);

    wanted.seed = 5;
    CHECK(seating.updateSettings(host, wanted) == ErrorCode::BadMessage);
    checkOneOwner(seating);
}
