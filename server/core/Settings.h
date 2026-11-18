#ifndef WHIST_CORE_SETTINGS_H
#define WHIST_CORE_SETTINGS_H

#include "core/Names.h"

#include <romanian_whist/Scoreboard.h>

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace whist
{

// The nine computer players, named as the catalogues name them.
enum class Level
{
    Random,
    FirstCard,
    LowRisk,
    Ducking,
    Cyborg,
    ReckonerAlpha,
    ReckonerBeta,
    ReckonerGamma,
    ReckonerDelta
};

std::string_view levelName(Level level);
std::optional<Level> parseLevel(std::string_view name);

enum class SeatKind
{
    Human,
    Computer,
    Open
};

enum class Mode
{
    Solo,
    Online
};

// A person's name, gender and portrait. The portrait is a catalogue id.
struct Profile
{
    std::string name;
    std::string gender;
    std::string avatar;

    bool operator==(const Profile&) const = default;
};

struct SeatSetting
{
    SeatKind kind = SeatKind::Open;

    // Computer seats only.
    std::optional<Level> level;

    // Human and computer seats.
    Profile profile;

    bool operator==(const SeatSetting&) const = default;
};

struct Settings
{
    std::vector<SeatSetting> seats;
    romanian_whist::GameStructure structure = romanian_whist::GameStructure::S_181;
    bool foreheadAndHidden = true;
    bool allOneCardForehead = false;

    // 0 (slow) to 4 (instant).
    int botSpeed = 2;
    bool waitAfterRound = false;

    // Solo only: a seed is the whole deal.
    std::optional<std::uint32_t> seed;

    // Online only. Who plays a human's seat while they cannot, and how long a turn may take.
    std::optional<Level> replacement;
    std::optional<std::chrono::milliseconds> turnTimer;

    bool operator==(const Settings&) const = default;
};

inline constexpr int slowestSpeed = 0;
inline constexpr int fastestSpeed = 4;

// Online turn timers. A timer of 0 is none.
inline constexpr std::chrono::milliseconds defaultTurnTimer{60000};
inline constexpr std::chrono::milliseconds minTurnTimer{10000};

struct SettingsError
{
    // The field at fault, for error.detail.
    std::string field;
};

// Everything the engine's start() would refuse is refused here, so a game never fails to start on
// its own thread. A game needs every seat filled, its names already cleaned, and every portrait
// one the catalogue gives that seat.
std::optional<SettingsError> checkSettings(const Settings& settings, Mode mode);

// The form of a catalogue id: lower-case letters, digits and '-', at most 40 characters.
bool isValidAvatarId(std::string_view avatar);

// An online table's settings when it is made: open and computer seats only, at least one open for
// its maker. Everything about the game is checked as for a game.
std::optional<SettingsError> checkNewTableSettings(const Settings& settings);

// An online table's settings apart from its seats: speed, structure, replacement, timer, no seed.
std::optional<SettingsError> checkOnlineRules(const Settings& settings);

// Online, no replacement means Reckoner Beta, the strongest of the affordable ones, and no turn
// timer means 60 s.
void completeOnline(Settings& settings);

// A person's profile: a clean name that a Reckoner would not take for a computer player's, and a
// portrait for their gender that is kept for no level.
std::optional<SettingsError> checkPersonProfile(const Profile& profile);
std::optional<SettingsError> checkComputerProfile(const Profile& profile, Level level);

// Cleans every name in place, as cleanName() does, or names the field at fault.
std::optional<SettingsError> cleanNames(Settings& settings);
std::optional<SettingsError> cleanProfile(Profile& profile);

// Cleans a person's profile, then checks it.
std::optional<SettingsError> preparePersonProfile(Profile& profile);

} // namespace whist

#endif
