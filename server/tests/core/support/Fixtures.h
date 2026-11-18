#ifndef WHIST_TESTS_CORE_FIXTURES_H
#define WHIST_TESTS_CORE_FIXTURES_H

#include <json/value.h>

#include <string>

namespace whist::test
{

// Compares a message with protocol/fixtures/<name>.json, or writes it there when the environment
// has WHIST_WRITE_FIXTURES=1. Returns what differs, or nothing.
std::string checkFixture(const std::string& name, const Json::Value& message);

// The pretty form a fixture file holds.
std::string fixtureText(const Json::Value& message);

} // namespace whist::test

#endif
