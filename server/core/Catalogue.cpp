#include "core/Catalogue.h"

#include <json/reader.h>
#include <json/value.h>

#include <algorithm>
#include <memory>
#include <stdexcept>

namespace whist
{

namespace catalogueData
{
extern const std::string_view playerNames;
extern const std::string_view avatars;
} // namespace catalogueData

namespace
{

Json::Value parse(std::string_view text, const char* what)
{
    Json::CharReaderBuilder builder;
    const std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
    Json::Value root;
    std::string errors;
    if(!reader->parse(text.data(), text.data() + text.size(), &root, &errors) || !root.isArray())
        throw std::runtime_error(std::string(what) + " is not a JSON array");
    return root;
}

[[noreturn]] void refuse(const char* what, Json::ArrayIndex index, const char* why)
{
    throw std::runtime_error(std::string(what) + " entry " + std::to_string(index) + ": " + why);
}

// "male", "female", or "any", which is kept as none.
std::optional<std::string> gender(const Json::Value& entry, const char* what, Json::ArrayIndex index)
{
    const Json::Value& value = entry["gender"];
    if(value == "male" || value == "female")
        return value.asString();
    if(value == "any")
        return std::nullopt;
    refuse(what, index, "bad gender");
}

std::string strategy(const Json::Value& entry, const char* what, Json::ArrayIndex index)
{
    if(!entry.isMember("strategy"))
        return {};
    const Json::Value& value = entry["strategy"];
    if(!value.isString() || !parseLevel(value.asString()))
        refuse(what, index, "bad strategy");
    return value.asString();
}

bool suitsGender(const std::optional<std::string>& entry, std::string_view gender)
{
    return !entry || *entry == gender;
}

} // namespace

const Catalogue& Catalogue::get()
{
    static const Catalogue catalogue(catalogueData::playerNames, catalogueData::avatars);
    return catalogue;
}

Catalogue::Catalogue(std::string_view namesJson, std::string_view avatarsJson)
{
    const Json::Value names = parse(namesJson, "player_names.json");
    std::set<std::pair<std::string, std::string>> seen;
    for(Json::ArrayIndex i = 0 ; i < names.size() ; i++)
    {
        const Json::Value& entry = names[i];
        if(!entry.isObject() || !entry["name"].isString() || !isCleanName(entry["name"].asString()))
            refuse("player_names.json", i, "bad name");

        Name name;
        name.name = entry["name"].asString();
        name.gender = gender(entry, "player_names.json", i);

        const Json::Value& language = entry["language"];
        if(language == "en" || language == "ro")
            name.language = language.asString();
        else if(language != "any")
            refuse("player_names.json", i, "bad language");

        name.strategy = strategy(entry, "player_names.json", i);

        // The same name may be in both languages, as Laura is, but not twice in one.
        if(!seen.emplace(name.language.value_or("any"), nameKey(name.name)).second)
            refuse("player_names.json", i, "a name twice in one language");
        names_.push_back(std::move(name));
    }

    const Json::Value avatars = parse(avatarsJson, "avatars.json");
    std::set<std::string> ids;
    for(Json::ArrayIndex i = 0 ; i < avatars.size() ; i++)
    {
        const Json::Value& entry = avatars[i];
        if(!entry.isObject() || !entry["id"].isString() || !isValidAvatarId(entry["id"].asString()))
            refuse("avatars.json", i, "bad id");
        if(!entry["file"].isString() || entry["file"].asString().empty())
            refuse("avatars.json", i, "no file");

        Avatar avatar;
        avatar.id = entry["id"].asString();
        avatar.gender = gender(entry, "avatars.json", i);
        avatar.strategy = strategy(entry, "avatars.json", i);
        if(!ids.insert(avatar.id).second)
            refuse("avatars.json", i, "an id twice");
        avatars_.push_back(std::move(avatar));
    }
}

const std::vector<Catalogue::Name>& Catalogue::names() const
{
    return names_;
}

const std::vector<Catalogue::Avatar>& Catalogue::avatars() const
{
    return avatars_;
}

bool Catalogue::suitsHuman(std::string_view avatar, std::string_view gender) const
{
    return std::ranges::any_of(avatars_, [&](const Avatar& entry)
    {
        return entry.id == avatar && entry.strategy.empty() && suitsGender(entry.gender, gender);
    });
}

bool Catalogue::suitsComputer(std::string_view avatar, std::string_view gender, Level level) const
{
    return std::ranges::any_of(avatars_, [&](const Avatar& entry)
    {
        return entry.id == avatar && (entry.strategy.empty() || entry.strategy == levelName(level)) &&
               suitsGender(entry.gender, gender);
    });
}

Profile Catalogue::drawComputer(Level level, std::string_view language, const std::optional<std::string>& gender,
                                unsigned int seatNumber, const std::set<std::string>& takenKeys,
                                const std::map<std::string, unsigned int>& worn, std::mt19937_64& random) const
{
    const auto pick = [&random](std::size_t count)
    {
        std::uniform_int_distribution<std::size_t> distribution(0, count - 1);
        return distribution(random);
    };

    Profile profile;
    profile.gender = gender ? *gender : (pick(2) == 0 ? "female" : "male");
    const std::string strategy(levelName(level));

    const auto freeNames = [&](const std::string& kept)
    {
        std::vector<std::string> found;
        for(const Name& name : names_)
        {
            if(name.strategy == kept && suitsGender(name.gender, profile.gender) &&
               (!name.language || *name.language == language) && !takenKeys.contains(nameKey(name.name)))
                found.push_back(name.name);
        }
        return found;
    };

    std::vector<std::string> names = freeNames(strategy);
    if(names.empty())
        names = freeNames({});
    if(!names.empty())
        profile.name = names[pick(names.size())];
    else
    {
        for(unsigned int number = std::max(1U, seatNumber) ; ; number++)
        {
            profile.name = "Bot " + std::to_string(number);
            if(!takenKeys.contains(nameKey(profile.name)))
                break;
        }
    }

    const auto group = [&](const std::string& kept)
    {
        std::vector<std::string> ids;
        for(const Avatar& avatar : avatars_)
        {
            if(avatar.strategy == kept && suitsGender(avatar.gender, profile.gender))
                ids.push_back(avatar.id);
        }
        return ids;
    };

    std::vector<std::string> theirs = group(strategy);
    if(theirs.empty())
        theirs = group({});
    if(theirs.empty())
        return profile;

    const auto wornCount = [&worn](const std::string& id)
    {
        const auto found = worn.find(id);
        return found == worn.end() ? 0U : found->second;
    };
    unsigned int least = wornCount(theirs.front());
    for(const std::string& id : theirs)
        least = std::min(least, wornCount(id));

    std::vector<std::string> leastWorn;
    for(const std::string& id : theirs)
    {
        if(wornCount(id) == least)
            leastWorn.push_back(id);
    }
    profile.avatar = leastWorn[pick(leastWorn.size())];
    return profile;
}

} // namespace whist
