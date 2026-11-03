#ifndef WHIST_TESTS_CORE_SERVERMESSAGES_H
#define WHIST_TESTS_CORE_SERVERMESSAGES_H

#include <json/value.h>

#include <string>

namespace whist::test
{

// Decodes a server message the way a client would, field by field, against PROTOCOL.md: every
// field it must have, of the right type, and none it must not. Empty when the message is right,
// otherwise what is wrong with it.
std::string shapeError(const Json::Value& message);

} // namespace whist::test

#endif
