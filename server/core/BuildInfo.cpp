#include "core/BuildInfo.h"

#include <romanian_whist/Version.h>

namespace whist
{

std::string_view serverVersion()
{
    return WHIST_WEB_VERSION;
}

std::string_view engineVersion()
{
    return romanian_whist::VersionString;
}

} // namespace whist
