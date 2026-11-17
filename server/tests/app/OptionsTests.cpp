#include "app/ServerOptions.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <json/json.h>

#include <fstream>
#include <stdexcept>
#include <string>

using whist::parseOptions;
using Catch::Matchers::ContainsSubstring;

namespace
{

Json::Value readJsonFile(const std::string& path)
{
    std::ifstream file(path, std::ios::binary);
    REQUIRE(file);

    Json::Value root;
    std::string errors;
    REQUIRE(Json::parseFromStream(Json::CharReaderBuilder(), file, &root, &errors));
    return root;
}

} // namespace

TEST_CASE("An empty config keeps every default", "[options]")
{
    const whist::ServerOptions options = parseOptions("{}");

    CHECK(options.address == "127.0.0.1");
    CHECK(options.port == 8080);
    CHECK(options.documentRoot == "server/www");
    CHECK(options.ioThreads == 2);
    CHECK(options.maxMessageBytes == 4096);
    CHECK(options.pingInterval == std::chrono::milliseconds(10000));
    CHECK(options.pongTimeout == std::chrono::milliseconds(30000));
    CHECK(options.reapInterval == std::chrono::milliseconds(1000));
    CHECK(options.statusLogInterval == std::chrono::milliseconds(60000));
    CHECK(options.gracePeriod == std::chrono::milliseconds(30000));
    CHECK(options.onlineIdle == std::chrono::minutes(10));
    CHECK(options.soloIdle == std::chrono::minutes(30));
    CHECK(options.forgetMember == std::chrono::hours(1));
    CHECK(options.drainTime == std::chrono::minutes(15));
    CHECK(options.instantFloor == std::chrono::milliseconds(30));
    CHECK(options.logLevel == "info");
    CHECK(options.messagesPerSecond == 20);
    CHECK(options.messageBurst == 40);
    CHECK(options.maxConnections == 2000);
    CHECK(options.maxConnectionsPerIp == 16);
    CHECK(options.tablesPerIpPerHour == 30);
    CHECK(options.failedJoinsPerIp == 10);
    CHECK(options.failedJoinWindow == std::chrono::minutes(10));
    CHECK(options.maxRunningGames == 50);
    CHECK(options.maxTables == 400);
    CHECK(options.trustedProxies == std::vector<std::string>{"127.0.0.1", "::1"});
}

TEST_CASE("The sample config says what the defaults are", "[options]")
{
    const std::string path = std::string(WHIST_SOURCE_DIR) + "/server/app/config.json";
    const whist::ServerOptions sample = whist::loadOptions(path);
    const whist::ServerOptions defaults;

    CHECK(sample.statusLogInterval == defaults.statusLogInterval);
    CHECK(sample.drainTime == defaults.drainTime);
    CHECK(sample.messagesPerSecond == defaults.messagesPerSecond);
    CHECK(sample.messageBurst == defaults.messageBurst);
    CHECK(sample.maxConnections == defaults.maxConnections);
    CHECK(sample.maxConnectionsPerIp == defaults.maxConnectionsPerIp);
    CHECK(sample.tablesPerIpPerHour == defaults.tablesPerIpPerHour);
    CHECK(sample.failedJoinsPerIp == defaults.failedJoinsPerIp);
    CHECK(sample.failedJoinWindow == defaults.failedJoinWindow);
    CHECK(sample.maxRunningGames == defaults.maxRunningGames);
    CHECK(sample.maxTables == defaults.maxTables);
    CHECK(sample.trustedProxies == defaults.trustedProxies);
}

TEST_CASE("A config overrides the keys it names", "[options]")
{
    const whist::ServerOptions options = parseOptions(R"({
        "address": "::1", "port": 9000, "document_root": "/srv/whist",
        "io_threads": 4, "max_message_bytes": 1024, "ping_interval_ms": 250,
        "pong_timeout_ms": 900, "reap_interval_ms": 50, "status_log_interval_ms": 250, "grace_period_ms": 700,
        "online_idle_ms": 8000, "solo_idle_ms": 9000, "forget_member_ms": 7000,
        "drain_ms": 61000, "instant_floor_ms": 0, "log_level": "debug",
        "messages_per_second": 5, "message_burst": 7, "max_connections": 100, "max_connections_per_ip": 3,
        "tables_per_ip_per_hour": 4, "failed_joins_per_ip": 2, "failed_join_window_ms": 60000,
        "max_running_games": 9, "max_tables": 13, "trusted_proxies": ["10.0.0.0/8", "2001:db8::/32", "192.0.2.7"]
    })");

    CHECK(options.address == "::1");
    CHECK(options.port == 9000);
    CHECK(options.documentRoot == "/srv/whist");
    CHECK(options.ioThreads == 4);
    CHECK(options.maxMessageBytes == 1024);
    CHECK(options.pingInterval == std::chrono::milliseconds(250));
    CHECK(options.pongTimeout == std::chrono::milliseconds(900));
    CHECK(options.reapInterval == std::chrono::milliseconds(50));
    CHECK(options.statusLogInterval == std::chrono::milliseconds(250));
    CHECK(options.gracePeriod == std::chrono::milliseconds(700));
    CHECK(options.onlineIdle == std::chrono::milliseconds(8000));
    CHECK(options.soloIdle == std::chrono::milliseconds(9000));
    CHECK(options.forgetMember == std::chrono::milliseconds(7000));
    CHECK(options.drainTime == std::chrono::milliseconds(61000));
    CHECK(options.instantFloor == std::chrono::milliseconds(0));
    CHECK(options.logLevel == "debug");
    CHECK(options.messagesPerSecond == 5);
    CHECK(options.messageBurst == 7);
    CHECK(options.maxConnections == 100);
    CHECK(options.maxConnectionsPerIp == 3);
    CHECK(options.tablesPerIpPerHour == 4);
    CHECK(options.failedJoinsPerIp == 2);
    CHECK(options.failedJoinWindow == std::chrono::milliseconds(60000));
    CHECK(options.maxRunningGames == 9);
    CHECK(options.maxTables == 13);
    CHECK(options.trustedProxies == std::vector<std::string>{"10.0.0.0/8", "2001:db8::/32", "192.0.2.7"});
}

TEST_CASE("The drain may be given no time, and the old delay is not a setting any more", "[options]")
{
    CHECK(parseOptions(R"({"drain_ms": 0})").drainTime == std::chrono::milliseconds(0));
    CHECK_THROWS_WITH(parseOptions(R"({"drain_ms": -1})"), ContainsSubstring("\"drain_ms\""));
    CHECK_THROWS_WITH(parseOptions(R"({"drain_ms": "900000"})"), ContainsSubstring("\"drain_ms\""));
    CHECK_THROWS_WITH(parseOptions(R"({"drain_ms": 90000000})"), ContainsSubstring("\"drain_ms\""));
    CHECK_THROWS_WITH(parseOptions(R"({"shutdown_delay_ms": 0})"), ContainsSubstring("\"shutdown_delay_ms\" is not a known setting"));
}

TEST_CASE("No proxy may be trusted at all", "[options]")
{
    CHECK(parseOptions(R"({"trusted_proxies": []})").trustedProxies.empty());
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
    CHECK_THROWS_WITH(parseOptions(R"({"reap_interval_ms": 0})"), ContainsSubstring("\"reap_interval_ms\""));
    CHECK_THROWS_WITH(parseOptions(R"({"pong_timeout_ms": 0})"), ContainsSubstring("\"pong_timeout_ms\""));
    CHECK_THROWS_WITH(parseOptions(R"({"grace_period_ms": 0})"), ContainsSubstring("\"grace_period_ms\""));
    CHECK_THROWS_WITH(parseOptions(R"({"online_idle_ms": "10"})"), ContainsSubstring("\"online_idle_ms\""));
    CHECK_THROWS_WITH(parseOptions(R"({"solo_idle_ms": -1})"), ContainsSubstring("\"solo_idle_ms\""));
    CHECK_THROWS_WITH(parseOptions(R"({"forget_member_ms": 0})"), ContainsSubstring("\"forget_member_ms\""));

    // A pong cannot come before its ping, whichever of the two the config moves.
    CHECK_THROWS_WITH(parseOptions(R"({"pong_timeout_ms": 10000})"), ContainsSubstring("\"pong_timeout_ms\""));
    CHECK_THROWS_WITH(parseOptions(R"({"ping_interval_ms": 40000})"), ContainsSubstring("\"pong_timeout_ms\""));
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

TEST_CASE("The load test's settings are the Docker image's, with the limits lifted and the stops short", "[options]")
{
    const std::string loadPath = std::string(WHIST_SOURCE_DIR) + "/deploy/load-config.json";
    Json::Value image = readJsonFile(std::string(WHIST_SOURCE_DIR) + "/deploy/docker-config.json");
    Json::Value load = readJsonFile(loadPath);
    const whist::ServerOptions options = whist::loadOptions(loadPath);

    // A load test is one address making hundreds of tables and connections, and answering at once.
    CHECK(options.messagesPerSecond >= 10000);
    CHECK(options.messageBurst >= 10000);
    CHECK(options.maxConnections >= 10000);
    CHECK(options.maxConnectionsPerIp >= 10000);
    CHECK(options.tablesPerIpPerHour >= 100000);
    CHECK(options.failedJoinsPerIp >= 10000);
    CHECK(options.maxRunningGames >= 1000);
    CHECK(options.maxTables >= 1000);

    // It reports often, to be read as it goes, and stops quickly, with games still running.
    CHECK(options.statusLogInterval <= std::chrono::seconds(10));
    CHECK(options.drainTime <= std::chrono::seconds(30));

    // Everything else is the image's, so the server that is measured is the one that is deployed.
    for(const char* key : {"messages_per_second", "message_burst", "max_connections", "max_connections_per_ip",
                           "tables_per_ip_per_hour", "failed_joins_per_ip", "max_running_games", "max_tables",
                           "status_log_interval_ms", "drain_ms"})
    {
        CHECK(load.isMember(key));
        load.removeMember(key);
        image.removeMember(key);
    }
    CHECK(load == image);
}

TEST_CASE("A limit is refused when it is 0, negative or not a number", "[options]")
{
    for(const char* key : {"messages_per_second", "message_burst", "max_connections", "max_connections_per_ip",
                           "tables_per_ip_per_hour", "failed_joins_per_ip", "failed_join_window_ms",
                           "max_running_games", "max_tables", "status_log_interval_ms"})
    {
        for(const char* value : {"0", "-1", "\"20\"", "true", "1.5", "null"})
        {
            const std::string config = std::string("{\"") + key + "\": " + value + "}";
            CAPTURE(config);
            CHECK_THROWS_WITH(parseOptions(config), ContainsSubstring(std::string("\"") + key + "\""));
        }
    }
    CHECK_THROWS_WITH(parseOptions(R"({"messages_per_second": 100001})"), ContainsSubstring("\"messages_per_second\""));
}

TEST_CASE("The trusted proxies are refused when one is not an address or a range", "[options]")
{
    CHECK_THROWS_WITH(parseOptions(R"({"trusted_proxies": "127.0.0.1"})"), ContainsSubstring("\"trusted_proxies\""));
    CHECK_THROWS_WITH(parseOptions(R"({"trusted_proxies": [127]})"), ContainsSubstring("\"trusted_proxies\""));
    CHECK_THROWS_WITH(parseOptions(R"({"trusted_proxies": ["127.0.0.1", "proxy.example.org"]})"),
                      ContainsSubstring("proxy.example.org"));
    CHECK_THROWS_WITH(parseOptions(R"({"trusted_proxies": ["10.0.0.0/33"]})"), ContainsSubstring("10.0.0.0/33"));
    CHECK_THROWS_WITH(parseOptions(R"({"trusted_proxies": [""]})"), ContainsSubstring("\"trusted_proxies\""));
}

TEST_CASE("The Docker image's settings are server/app/config.json's, listening on every address", "[options]")
{
    const std::string imagePath = std::string(WHIST_SOURCE_DIR) + "/deploy/docker-config.json";
    Json::Value local = readJsonFile(std::string(WHIST_SOURCE_DIR) + "/server/app/config.json");
    Json::Value image = readJsonFile(imagePath);

    CHECK(image["address"].asString() == "0.0.0.0");
    CHECK(image["document_root"].asString() == "/opt/whist/www");

    // The image's proxies are the compose network's: Caddy reaches the server from a private address.
    CHECK(whist::loadOptions(imagePath).trustedProxies
          == std::vector<std::string>{"127.0.0.1", "::1", "10.0.0.0/8", "172.16.0.0/12", "192.168.0.0/16"});

    // Whatever else one file sets, the other must too, so a new setting reaches the image.
    for(const char* key : {"address", "document_root", "trusted_proxies"})
    {
        local.removeMember(key);
        image.removeMember(key);
    }
    CHECK(image == local);

    CHECK_NOTHROW(whist::loadOptions(imagePath));
}
