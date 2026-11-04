#include "app/ServerOptions.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <stdexcept>

using whist::parseOptions;
using Catch::Matchers::ContainsSubstring;

TEST_CASE("An empty config keeps every default", "[options]")
{
    const whist::ServerOptions options = parseOptions("{}");

    CHECK(options.address == "127.0.0.1");
    CHECK(options.port == 8080);
    CHECK(options.documentRoot == "server/www");
    CHECK(options.ioThreads == 2);
    CHECK(options.maxMessageBytes == 4096);
    CHECK(options.pingInterval == std::chrono::milliseconds(10000));
    CHECK(options.shutdownDelay == std::chrono::milliseconds(0));
    CHECK(options.instantFloor == std::chrono::milliseconds(30));
    CHECK(options.logLevel == "info");
}

TEST_CASE("A config overrides the keys it names", "[options]")
{
    const whist::ServerOptions options = parseOptions(R"({
        "address": "::1", "port": 9000, "document_root": "/srv/whist",
        "io_threads": 4, "max_message_bytes": 1024, "ping_interval_ms": 250,
        "shutdown_delay_ms": 900000, "instant_floor_ms": 0, "log_level": "debug"
    })");

    CHECK(options.address == "::1");
    CHECK(options.port == 9000);
    CHECK(options.documentRoot == "/srv/whist");
    CHECK(options.ioThreads == 4);
    CHECK(options.maxMessageBytes == 1024);
    CHECK(options.pingInterval == std::chrono::milliseconds(250));
    CHECK(options.shutdownDelay == std::chrono::milliseconds(900000));
    CHECK(options.instantFloor == std::chrono::milliseconds(0));
    CHECK(options.logLevel == "debug");
}

TEST_CASE("A config is refused for what is wrong with it", "[options]")
{
    CHECK_THROWS_WITH(parseOptions(R"({"prot": 8080})"), ContainsSubstring("\"prot\" is not a known setting"));
    CHECK_THROWS_WITH(parseOptions(R"({"port": "8080"})"), ContainsSubstring("\"port\""));
    CHECK_THROWS_WITH(parseOptions(R"({"port": 70000})"), ContainsSubstring("\"port\""));
    CHECK_THROWS_WITH(parseOptions(R"({"port": -1})"), ContainsSubstring("\"port\""));
    CHECK_THROWS_WITH(parseOptions(R"({"port": 0})"), ContainsSubstring("\"port\""));
    CHECK_THROWS_WITH(parseOptions(R"({"io_threads": 0})"), ContainsSubstring("\"io_threads\""));
    CHECK_THROWS_WITH(parseOptions(R"({"ping_interval_ms": 0})"), ContainsSubstring("\"ping_interval_ms\""));
    CHECK_THROWS_WITH(parseOptions(R"({"ping_interval_ms": true})"), ContainsSubstring("\"ping_interval_ms\""));
    CHECK_THROWS_WITH(parseOptions(R"({"address": ""})"), ContainsSubstring("\"address\""));
    CHECK_THROWS_WITH(parseOptions(R"({"address": "localhost"})"), ContainsSubstring("must be an IP address"));
    CHECK_THROWS_WITH(parseOptions(R"({"address": "127.0.0.256"})"), ContainsSubstring("must be an IP address"));
    CHECK_THROWS_WITH(parseOptions(R"({"log_level": "loud"})"), ContainsSubstring("\"log_level\""));
    CHECK_THROWS_WITH(parseOptions(R"({"instant_floor_ms": 5000})"), ContainsSubstring("\"instant_floor_ms\""));
    CHECK_THROWS_WITH(parseOptions("[]"), ContainsSubstring("must be a JSON object"));
    CHECK_THROWS_WITH(parseOptions("{"), ContainsSubstring("not valid JSON"));
    CHECK_THROWS_WITH(parseOptions(R"({"port": 1, "port": 2})"), ContainsSubstring("not valid JSON"));
    CHECK_THROWS_AS(whist::loadOptions("/nonexistent/whist.json"), std::invalid_argument);
}
