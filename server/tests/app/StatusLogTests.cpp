#include "ServerProcess.h"
#include "TestSupport.h"

#include <catch2/catch_test_macros.hpp>

#include <functional>
#include <map>
#include <optional>
#include <sstream>
#include <string>

using namespace std::chrono_literals;
using namespace whist::test;

namespace
{

// The pairs of a status line, by key.
std::map<std::string, std::string> pairsOf(const std::string& line)
{
    std::map<std::string, std::string> pairs;
    std::istringstream words(line);
    for(std::string word ; words >> word ; )
    {
        const std::size_t equals = word.find('=');
        if(equals != std::string::npos)
            pairs[word.substr(0, equals)] = word.substr(equals + 1);
    }
    return pairs;
}

} // namespace

TEST_CASE("The log says what the server is doing, once in a while", "[status][process]")
{
    TempDir dir;
    const std::uint16_t port = freePort();
    dir.write("index.html", "<!doctype html><title>test</title>\n");
    dir.write("config.json", R"({"port": )" + std::to_string(port) + R"(, "status_log_interval_ms": 200, "document_root": ")"
                             + dir.path().string() + R"("})");

    ServerProcess server((dir.path() / "config.json").string());
    const bool listening = server.waitForLine("listening on ", 10s).has_value();
    INFO(server.log());
    REQUIRE(listening);

    // The first line: nobody here.
    const std::optional<std::string> first = server.waitForLine("status ", 5s);
    REQUIRE(first);
    std::map<std::string, std::string> pairs = pairsOf(*first);
    CHECK(pairs.at("connections") == "0");
    CHECK(pairs.at("tables_lobby") == "0");
    CHECK(pairs.at("tables_playing") == "0");
    CHECK(pairs.at("tables_finished") == "0");
    CHECK(pairs.at("game_threads") == "0");
    CHECK(pairs.at("stand_in_moves") == "0");
    CHECK(std::stod(pairs.at("cpu_percent")) >= 0.0);
    CHECK(std::stod(pairs.at("cpu_seconds")) >= 0.0);
    CHECK(std::stoul(pairs.at("peak_memory_mb")) > 0);

    // A person with a game that waits for them.
    TestClient player(port, "/ws");
    player.send(R"({"type":"hello","protocol":2,"lang":"en"})");
    REQUIRE(player.waitForMessages(2, 5s));
    player.send(R"({"type":"createSolo","settings":{"structure":"1-8-1","seats":[)"
                R"({"kind":"human","name":"Ana","gender":"female","avatar":"woman-long-red-hair"},)"
                R"({"kind":"computer","level":"cyborg","name":"Bogdan","gender":"male","avatar":"man-dark-hair-beard"}]}})");
    REQUIRE(player.waitFor([](const Json::Value& m) { return m["type"] == "prompt"; }, 5s));

    const auto next = [&](const std::function<bool(const std::map<std::string, std::string>&)>& wanted)
    {
        for(int attempt = 0 ; attempt < 50 ; attempt++)
        {
            const std::optional<std::string> line = server.waitForLine("status ", 5s);
            if(!line)
                return false;
            pairs = pairsOf(*line);
            if(wanted(pairs))
                return true;
        }
        return false;
    };

    CHECK(next([](const auto& p)
    {
        return p.at("connections") == "1" && p.at("tables_playing") == "1" && p.at("game_threads") == "1";
    }));

    player.close();
    CHECK(next([](const auto& p) { return p.at("connections") == "0"; }));
}
