#include "core/Settings.h"

#include <algorithm>
#include <array>
#include <cctype>
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

// Decodes one code point, or returns false for malformed, overlong or surrogate sequences.
bool nextCodePoint(std::string_view text, std::size_t& at, char32_t& codePoint)
{
    const auto byte = [&](std::size_t i) { return static_cast<unsigned char>(text[i]); };
    const unsigned char lead = byte(at);

    std::size_t length = 0;
    char32_t minimum = 0;
    if(lead < 0x80)
    {
        codePoint = lead;
        at += 1;
        return true;
    }
    if((lead & 0xE0) == 0xC0)
    {
        length = 2;
        minimum = 0x80;
        codePoint = lead & 0x1F;
    }
    else if((lead & 0xF0) == 0xE0)
    {
        length = 3;
        minimum = 0x800;
        codePoint = lead & 0x0F;
    }
    else if((lead & 0xF8) == 0xF0)
    {
        length = 4;
        minimum = 0x10000;
        codePoint = lead & 0x07;
    }
    else
        return false;

    if(at + length > text.size())
        return false;

    for(std::size_t i = 1 ; i < length ; i++)
    {
        if((byte(at + i) & 0xC0) != 0x80)
            return false;
        codePoint = (codePoint << 6) | (byte(at + i) & 0x3F);
    }

    if(codePoint < minimum || codePoint > 0x10FFFF || (codePoint >= 0xD800 && codePoint <= 0xDFFF))
        return false;

    at += length;
    return true;
}

bool isControlOrBidi(char32_t c)
{
    if(c < 0x20 || (c >= 0x7F && c <= 0x9F))
        return true;

    return c == 0x061C || c == 0x200E || c == 0x200F || (c >= 0x202A && c <= 0x202E) ||
           (c >= 0x2066 && c <= 0x2069);
}

std::string asciiLower(std::string_view text)
{
    std::string lower(text);
    for(char& c : lower)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return lower;
}

bool isValidGender(std::string_view gender)
{
    return gender == "female" || gender == "male";
}

bool isValidAvatar(std::string_view avatar)
{
    if(avatar.empty() || avatar.size() > 40)
        return false;

    return std::ranges::all_of(avatar, [](char c)
    {
        return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-';
    });
}

} // namespace

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

bool isValidUtf8(std::string_view text)
{
    std::size_t at = 0;
    char32_t codePoint = 0;
    while(at < text.size())
    {
        if(!nextCodePoint(text, at, codePoint))
            return false;
    }
    return true;
}

bool isValidName(std::string_view name)
{
    std::size_t at = 0;
    std::size_t length = 0;
    bool visible = false;
    char32_t codePoint = 0;

    while(at < name.size())
    {
        if(!nextCodePoint(name, at, codePoint) || isControlOrBidi(codePoint))
            return false;
        if(codePoint != U' ')
            visible = true;
        length++;
    }

    return visible && length <= maxNameLength;
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
        if(!isValidName(profile.name))
            return SettingsError{"name"};
        if(!isValidGender(profile.gender))
            return SettingsError{"gender"};
        if(!isValidAvatar(profile.avatar))
            return SettingsError{"avatar"};

        // The engine wants names that differ byte for byte; people want them to differ to the eye.
        if(!names.insert(asciiLower(profile.name)).second)
            return SettingsError{"name"};

        if(seat.kind == SeatKind::Human)
        {
            humans++;

            // The Reckoner models any seat so named as a deterministic player, whoever sits there.
            const std::string lower = asciiLower(profile.name);
            if(lower.find("lowrisk") != std::string::npos || lower.find("ducking") != std::string::npos)
                return SettingsError{"name"};
        }
    }

    if(mode == Mode::Solo && humans > 1)
        return SettingsError{"seats"};

    return std::nullopt;
}

} // namespace whist
