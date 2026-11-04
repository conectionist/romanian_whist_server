#include "core/Settings.h"

#include "core/Catalogue.h"

#include <algorithm>
#include <array>
#include <set>
#include <utility>

namespace whist
{

namespace
{

constexpr std::array<std::pair<Level, std::string_view>, 9> levelNames{{
    {Level::Random, "random"},
    {Level::FirstCard, "first-card"},
    {Level::LowRisk, "low-risk"},
    {Level::Ducking, "ducking"},
    {Level::Cyborg, "cyborg"},
    {Level::ReckonerAlpha, "reckoner-alpha"},
    {Level::ReckonerBeta, "reckoner-beta"},
    {Level::ReckonerGamma, "reckoner-gamma"},
    {Level::ReckonerDelta, "reckoner-delta"},
}};

// The Reckoner models any seat so named as a deterministic player, whoever sits there.
bool isReservedName(std::string_view name)
{
    const std::string key = nameKey(name);
    return key.find("lowrisk") != std::string::npos || key.find("ducking") != std::string::npos;
}

bool isValidGender(std::string_view gender)
{
    return gender == "female" || gender == "male";
}

} // namespace

bool isValidAvatarId(std::string_view avatar)
{
    if(avatar.empty() || avatar.size() > 40)
        return false;

    return std::ranges::all_of(avatar, [](char c)
    {
        return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-';
    });
}

std::string_view levelName(Level level)
{
    for(const auto& [value, name] : levelNames)
    {
        if(value == level)
            return name;
    }
    return "low-risk";
}

std::optional<Level> parseLevel(std::string_view name)
{
    for(const auto& [value, known] : levelNames)
    {
        if(known == name)
            return value;
    }
    return std::nullopt;
}

namespace
{

std::optional<SettingsError> checkPacingAndRules(const Settings& settings, Mode mode)
{
    if(settings.seats.size() < 2 || settings.seats.size() > 6)
        return SettingsError{"seats"};

    if(settings.botSpeed < slowestSpeed || settings.botSpeed > fastestSpeed)
        return SettingsError{"botSpeed"};

    if(mode == Mode::Solo && (settings.replacement || settings.turnTimer))
        return SettingsError{settings.replacement ? "replacement" : "turnTimer"};

    if(mode == Mode::Online && settings.seed)
        return SettingsError{"seed"};

    // A turn that times out is played by the seat's computer player, so a timer needs one. A short
    // one would let the computer player make every move.
    if(settings.turnTimer && settings.turnTimer->count() != 0 &&
       (!settings.replacement || *settings.turnTimer < minTurnTimer))
        return SettingsError{"turnTimer"};

    return std::nullopt;
}

// Every seat that is not open, and their names against each other.
std::optional<SettingsError> checkSeats(const Settings& settings)
{
    std::set<std::string> names;
    for(const SeatSetting& seat : settings.seats)
    {
        if(seat.kind == SeatKind::Open)
            continue;

        if(seat.kind == SeatKind::Computer && !seat.level)
            return SettingsError{"level"};
        if(seat.kind == SeatKind::Human && seat.level)
            return SettingsError{"level"};

        const std::optional<SettingsError> profile = seat.kind == SeatKind::Human
                                                     ? checkPersonProfile(seat.profile)
                                                     : checkComputerProfile(seat.profile, *seat.level);
        if(profile)
            return profile;

        // The engine wants names that differ byte for byte; people want them to differ to the eye.
        if(!names.insert(nameKey(seat.profile.name)).second)
            return SettingsError{"name"};
    }
    return std::nullopt;
}

} // namespace

std::optional<SettingsError> checkPersonProfile(const Profile& profile)
{
    if(!isCleanName(profile.name) || isReservedName(profile.name))
        return SettingsError{"name"};
    if(!isValidGender(profile.gender))
        return SettingsError{"gender"};
    if(!Catalogue::get().suitsHuman(profile.avatar, profile.gender))
        return SettingsError{"avatar"};
    return std::nullopt;
}

std::optional<SettingsError> checkComputerProfile(const Profile& profile, Level level)
{
    // Only the deterministic players themselves may be named so.
    if(!isCleanName(profile.name) ||
       (level != Level::LowRisk && level != Level::Ducking && isReservedName(profile.name)))
        return SettingsError{"name"};
    if(!isValidGender(profile.gender))
        return SettingsError{"gender"};
    if(!Catalogue::get().suitsComputer(profile.avatar, profile.gender, level))
        return SettingsError{"avatar"};
    return std::nullopt;
}

std::optional<SettingsError> checkSettings(const Settings& settings, Mode mode)
{
    if(const std::optional<SettingsError> error = checkPacingAndRules(settings, mode))
        return error;

    std::size_t humans = 0;
    for(const SeatSetting& seat : settings.seats)
    {
        if(seat.kind == SeatKind::Open)
            return SettingsError{"seats"};
        if(seat.kind == SeatKind::Human)
            humans++;
    }
    if(mode == Mode::Solo && humans > 1)
        return SettingsError{"seats"};

    return checkSeats(settings);
}

std::optional<SettingsError> checkNewTableSettings(const Settings& settings)
{
    if(const std::optional<SettingsError> error = checkPacingAndRules(settings, Mode::Online))
        return error;

    // People take their seats by joining.
    if(std::ranges::any_of(settings.seats, [](const SeatSetting& seat) { return seat.kind == SeatKind::Human; }))
        return SettingsError{"seats"};
    if(std::ranges::none_of(settings.seats, [](const SeatSetting& seat) { return seat.kind == SeatKind::Open; }))
        return SettingsError{"seats"};

    return checkSeats(settings);
}

std::optional<SettingsError> checkOnlineRules(const Settings& settings)
{
    return checkPacingAndRules(settings, Mode::Online);
}

void completeOnline(Settings& settings)
{
    if(!settings.replacement)
        settings.replacement = Level::ReckonerBeta;
    if(!settings.turnTimer)
        settings.turnTimer = defaultTurnTimer;
}

std::optional<SettingsError> cleanProfile(Profile& profile)
{
    std::optional<std::string> cleaned = cleanName(profile.name);
    if(!cleaned)
        return SettingsError{"name"};
    profile.name = std::move(*cleaned);
    return std::nullopt;
}

std::optional<SettingsError> preparePersonProfile(Profile& profile)
{
    if(const std::optional<SettingsError> error = cleanProfile(profile))
        return error;
    return checkPersonProfile(profile);
}

std::optional<SettingsError> cleanNames(Settings& settings)
{
    for(SeatSetting& seat : settings.seats)
    {
        if(seat.kind == SeatKind::Open)
            continue;
        std::optional<std::string> cleaned = cleanName(seat.profile.name);
        if(!cleaned)
            return SettingsError{"name"};
        seat.profile.name = std::move(*cleaned);
    }
    return std::nullopt;
}

} // namespace whist
