#ifndef WHIST_CORE_NAMES_H
#define WHIST_CORE_NAMES_H

#include <cstddef>
#include <optional>
#include <set>
#include <string>
#include <string_view>

namespace whist
{

inline constexpr std::size_t maxNameLength = 20;

bool isValidUtf8(std::string_view text);

// Code points, not bytes: "Ș" is one.
std::size_t codePointCount(std::string_view text);

// A name as a table shows it: NFC, every space a plain one, runs of them collapsed, none at either
// end, and invisible characters dropped except the joiners and variation selectors emoji are made
// of. None when it holds a control or bidirectional formatting character, which can make one name
// display as another, when nothing in it is visible, or when it is not 1 to 20 characters.
std::optional<std::string> cleanName(std::string_view name);

// True when cleanName() leaves the name as it is.
bool isCleanName(std::string_view name);

// What names are compared by: two names clash when their keys are equal. Case, width and invisible
// characters are ignored, and Romanian's cedilla letters count as its comma-below ones (Ş and Ș),
// since both are typed for the same name.
std::string nameKey(std::string_view name);

// The name, or failing that "Ana 2", "Ana 3" and on: the first whose key is not taken. The name is
// cut to keep within 20 characters. Expects a clean name.
std::string numberedName(std::string_view name, const std::set<std::string>& takenKeys);

} // namespace whist

#endif
