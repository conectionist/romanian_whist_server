#ifndef WHIST_LOOK_LOOKFIXTURES_H
#define WHIST_LOOK_LOOKFIXTURES_H

#include "core/Settings.h"

#include <json/value.h>

#include <chrono>
#include <filesystem>
#include <string>
#include <vector>

namespace whist::look
{

struct LookFile
{
    // The file's name in protocol/look/.
    std::string name;
    Json::Value content;

    // What the picture shows, for the line the tool prints.
    std::string moment;
};

struct Built
{
    std::vector<LookFile> files;

    // Everything that is not as the desktop's tool has it, in words. No file may be written while
    // there is one.
    std::vector<std::string> problems;
};

// The settings of the desktop's screenshot game at a table of 2 to 6 players, seat 0 the person.
Settings shotSettings(unsigned int players);

// Plays the desktop's games, and builds the hands and the empty table, as protocol/look/ holds them.
// `patience` is how long a game may stay silent before it is given up.
Built buildLookFiles(std::chrono::milliseconds patience = std::chrono::seconds(60));

// The text a file holds.
std::string lookText(const Json::Value& content);

// Where protocol/look/ is in the source tree.
std::filesystem::path lookDirectory();

} // namespace whist::look

#endif
