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

struct SettingsError
{
    // The field at fault, for error.detail.
    std::string field;
};

// Everything the engine's start() would refuse is refused here, so a game never fails to start on
// its own thread. A game needs every seat filled, and its names clean: see cleanNames().
std::optional<SettingsError> checkSettings(const Settings& settings, Mode mode);

// Cleans every name in place, as cleanName() does, or names the field at fault.
std::optional<SettingsError> cleanNames(Settings& settings);

} // namespace whist

#endif
