#ifndef WHIST_CORE_BUILDINFO_H
#define WHIST_CORE_BUILDINFO_H

#include <string_view>

namespace whist
{

std::string_view serverVersion();
std::string_view engineVersion();

} // namespace whist

#endif
