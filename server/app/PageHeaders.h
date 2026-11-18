#ifndef WHIST_APP_PAGEHEADERS_H
#define WHIST_APP_PAGEHEADERS_H

#include <string_view>

namespace whist
{

// The headers the server sends on every answer. The page's tests read the policy out of this file, so
// it stays one string literal.
inline constexpr std::string_view contentSecurityPolicy =
    "default-src 'self'; script-src 'self'; style-src 'self'; img-src 'self'; font-src 'self'; connect-src 'self'; object-src 'none'; base-uri 'none'; form-action 'self'; frame-ancestors 'none'";

inline constexpr std::string_view contentTypeOptions = "nosniff";
inline constexpr std::string_view referrerPolicy = "no-referrer";

} // namespace whist

#endif
