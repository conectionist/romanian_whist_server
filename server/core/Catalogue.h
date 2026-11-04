#ifndef WHIST_CORE_CATALOGUE_H
#define WHIST_CORE_CATALOGUE_H

#include "core/Settings.h"

#include <map>
#include <optional>
#include <random>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace whist
{

// Players' names and portraits, from protocol/catalogues/, which copy the desktop client's.
class Catalogue
{
public:
    struct Name
    {
        std::string name;

        // None means any.
        std::optional<std::string> gender;
        std::optional<std::string> language;

        // The level it is kept for, or empty for anyone.
        std::string strategy;
    };

    struct Avatar
    {
        std::string id;
        std::optional<std::string> gender;
        std::string strategy;
    };

    // The catalogues this server was built with.
    static const Catalogue& get();

    // Throws std::runtime_error on an entry the desktop client would refuse.
    Catalogue(std::string_view namesJson, std::string_view avatarsJson);

    const std::vector<Name>& names() const;
    const std::vector<Avatar>& avatars() const;

    // A portrait a person may choose: kept for no level, and for their gender or either.
    bool suitsHuman(std::string_view avatar, std::string_view gender) const;

    // A computer player's portrait: kept for its level or for none, and for its gender or either.
    bool suitsComputer(std::string_view avatar, std::string_view gender, Level level) const;

    // What a computer player gets, by the desktop client's rules. The gender is drawn when none is
    // given. The name is one kept for the level, else anyone's, else "Bot N" counting up from
    // `seatNumber`, never one whose key is taken. The portrait is the level's own, else anyone's,
    // picked among those worn least.
    Profile drawComputer(Level level, std::string_view language, const std::optional<std::string>& gender,
                         unsigned int seatNumber, const std::set<std::string>& takenKeys,
                         const std::map<std::string, unsigned int>& worn, std::mt19937_64& random) const;

private:
    std::vector<Name> names_;
    std::vector<Avatar> avatars_;
};

} // namespace whist

#endif
