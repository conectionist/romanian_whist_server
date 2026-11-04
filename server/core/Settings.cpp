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

std::optional<SettingsError> checkSettings(const Settings& settings, Mode mode)
{
    if(settings.seats.size() < 2 || settings.seats.size() > 6)
        return SettingsError{"seats"};

    if(settings.botSpeed < slowestSpeed || settings.botSpeed > fastestSpeed)
        return SettingsError{"botSpeed"};

    if(mode == Mode::Solo && (settings.replacement || settings.turnTimer))
        return SettingsError{settings.replacement ? "replacement" : "turnTimer"};

    if(mode == Mode::Online && settings.seed)
        return SettingsError{"seed"};

    // A turn that times out is played by the seat's computer player, so a timer needs one.
    if(settings.turnTimer && (!settings.replacement || settings.turnTimer->count() <= 0))
        return SettingsError{"turnTimer"};

    std::size_t humans = 0;
    std::set<std::string> names;

    for(const SeatSetting& seat : settings.seats)
    {
        if(seat.kind == SeatKind::Open)
            return SettingsError{"seats"};

        if(seat.kind == SeatKind::Computer && !seat.level)
            return SettingsError{"level"};
        if(seat.kind == SeatKind::Human && seat.level)
            return SettingsError{"level"};

        const Profile& profile = seat.profile;
        if(!isCleanName(profile.name))
            return SettingsError{"name"};
        if(!isValidGender(profile.gender))
            return SettingsError{"gender"};
        const Catalogue& catalogue = Catalogue::get();
        if(seat.kind == SeatKind::Human ? !catalogue.suitsHuman(profile.avatar, profile.gender)
                                        : !catalogue.suitsComputer(profile.avatar, profile.gender, *seat.level))
            return SettingsError{"avatar"};

        // The engine wants names that differ byte for byte; people want them to differ to the eye.
        if(!names.insert(nameKey(profile.name)).second)
            return SettingsError{"name"};

        if(seat.kind == SeatKind::Human)
        {
            humans++;
            if(isReservedName(profile.name))
                return SettingsError{"name"};
        }
    }

    if(mode == Mode::Solo && humans > 1)
        return SettingsError{"seats"};

    return std::nullopt;
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
