#ifndef WHIST_CORE_BUILDINFO_H
#define WHIST_CORE_BUILDINFO_H

#include <string_view>

namespace whist
{

std::string_view serverVersion();
std::string_view engineVersion();

// The commit this build was configured from, or empty when git could not say.
std::string_view serverCommit();

} // namespace whist

#endif
