#include "ServerProcess.h"
#include "TestSupport.h"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <csignal>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

using namespace std::chrono_literals;
using namespace whist::test;

namespace
{

using Clock = std::chrono::steady_clock;

// The real server as a process of its own, which a test stops with a signal. `extra` is more of the
// configuration, as JSON members after a comma.
struct Server
{
    TempDir dir;
    std::uint16_t port = freePort();
    std::unique_ptr<ServerProcess> process;

    explicit Server(const std::string& extra)
    {
        dir.write("index.html", "<!doctype html><title>test</title>\n");
        dir.write("config.json", R"({"port": )" + std::to_string(port) + R"(, "document_root": ")" + dir.path().string()
                                 + R"(")" + extra + "}");
        process = std::make_unique<ServerProcess>((dir.path() / "config.json").string());
        const bool listening = process->waitForLine("listening on ", 10s).has_value();
        INFO(process->log());
        REQUIRE(listening);
    }
};

const std::string hello = R"({"type":"hello","protocol":2,"lang":"en"})";

const std::string humanGame =
    R"({"type":"createSolo","settings":{"structure":"1-8-1","seats":[)"
    R"({"kind":"human","name":"Ana","gender":"female","avatar":"woman-long-red-hair"},)"
    R"({"kind":"computer","level":"cyborg","name":"Bogdan","gender":"male","avatar":"man-dark-hair-beard"}]}})";

// Two computer players at the fastest speed: a game that ends by itself within a few seconds, with
// the server's instant floor at 10 ms.
const std::string watchedGame =
    R"({"type":"createSolo","settings":{"structure":"1-8-1","botSpeed":4,"seats":[)"
    R"({"kind":"computer","level":"cyborg","name":"Ana","gender":"female","avatar":"woman-long-red-hair"},)"
    R"({"kind":"computer","level":"cyborg","name":"Bogdan","gender":"male","avatar":"man-dark-hair-beard"}]}})";

const std::string onlineTable =
    R"({"type":"createTable","profile":{"name":"Ana","gender":"female","avatar":"woman-long-red-hair"},"settings":{)"
    R"("structure":"1-8-1","seats":[{"kind":"open"},{"kind":"open"},)"
    R"({"kind":"computer","level":"cyborg","name":"Bogdan","gender":"male","avatar":"man-dark-hair-beard"}]}})";

bool isType(const Json::Value& message, const std::string& type)
{
    return message["type"] == type;
}

void greet(TestClient& client)
{
    client.send(hello);
    REQUIRE(client.waitFor([](const Json::Value& m) { return isType(m, "sync"); }, 5s));
}

std::chrono::milliseconds since(Clock::time_point start)
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start);
}

} // namespace

TEST_CASE("With no game running, a stop quits at once, whatever the drain time", "[drain][process]")
{
    Server server(R"(, "drain_ms": 60000)");

    TestClient watcher(server.port, "/ws");
    greet(watcher);

    const auto asked = Clock::now();
    server.process->signal(SIGTERM);
    CHECK(server.process->waitForLine("stop asked for: no new games, 0 running may finish within 60 s", 5s));
    CHECK(server.process->waitForLine("drained: no game is running", 5s));
    CHECK(server.process->waitForLine("shutting down", 5s));
    CHECK(server.process->waitForExit(5s) == 0);
    INFO(server.process->log());
    CHECK(since(asked) < 4s);
    CHECK(watcher.waitForClose(5s));
}

TEST_CASE("A game that ends before the deadline is waited for, and the server quits when it is over", "[drain][process]")
{
    Server server(R"(, "drain_ms": 30000, "instant_floor_ms": 10)");

    TestClient watcher(server.port, "/ws");
    greet(watcher);
    watcher.send(watchedGame);
    REQUIRE(watcher.waitFor([](const Json::Value& m) { return isType(m, "game"); }, 5s));

    const auto asked = Clock::now();
    server.process->signal(SIGTERM);
    CHECK(server.process->waitForLine("stop asked for: no new games, 1 running may finish within 30 s", 5s));

    // The watcher is told, and sees the game through to its end.
    const auto told = watcher.waitFor([](const Json::Value& m) { return isType(m, "restarting"); }, 5s);
    REQUIRE(told);
    CHECK(watcher.json()[*told]["deadline"].asInt64() <= 30000);
    CHECK(watcher.waitFor([](const Json::Value& m)
    {
        return isType(m, "game") && m["event"]["kind"] == "gameOver";
    }, 20s));

    // It quit when the game did, which is long before the thirty seconds.
    CHECK(server.process->waitForLine("drained: no game is running", 20s));
    CHECK(server.process->waitForExit(5s) == 0);
    INFO(server.process->log());
    CHECK(since(asked) < 20s);
    CHECK(server.process->log().find("drain time is up") == std::string::npos);
    CHECK(watcher.waitForClose(5s));
}

TEST_CASE("A game still running at the deadline is ended, and the server quits with 0", "[drain][process]")
{
    Server server(R"(, "drain_ms": 3000)");

    TestClient player(server.port, "/ws");
    greet(player);
    player.send(humanGame);
    REQUIRE(player.waitFor([](const Json::Value& m) { return isType(m, "prompt"); }, 5s));

    // A table that waits for players, which someone else can join during the drain.
    TestClient host(server.port, "/ws");
    greet(host);
    host.send(onlineTable);
    const auto made = host.waitFor([](const Json::Value& m) { return isType(m, "sync") && m["table"].isObject(); }, 5s);
    REQUIRE(made);
    const std::string code = host.json()[*made]["table"]["code"].asString();

    TestClient newcomer(server.port, "/ws");
    greet(newcomer);

    const auto asked = Clock::now();
    server.process->signal(SIGTERM);
    REQUIRE(server.process->waitForLine("stop asked for: no new games, 1 running may finish within 3 s", 5s));

    // Everyone who was connected is told, with what is left.
    const auto first = player.waitFor([](const Json::Value& m) { return isType(m, "restarting"); }, 5s);
    REQUIRE(first);
    const Json::Value restarting = player.json()[*first];
    CHECK(restarting["deadline"].asInt64() <= 3000);
    CHECK(restarting["deadline"].asInt64() > 2000);
    CHECK(newcomer.waitFor([](const Json::Value& m) { return isType(m, "restarting"); }, 5s));
    CHECK(host.waitFor([](const Json::Value& m) { return isType(m, "restarting"); }, 5s));

    // Nothing new starts: not a solo game, not an online table, and the page's health check says so.
    const auto refused = [](const Json::Value& m) { return isType(m, "error") && m["code"] == "draining"; };
    std::size_t mark = newcomer.messages().size();
    newcomer.send(humanGame);
    CHECK(newcomer.waitFor(refused, 5s, mark));
    mark = newcomer.messages().size();
    newcomer.send(onlineTable);
    CHECK(newcomer.waitFor(refused, 5s, mark));
    const auto [healthResult, health] = fetch(server.port, "/health");
    REQUIRE(healthResult == drogon::ReqResult::Ok);
    CHECK(health->statusCode() == drogon::k503ServiceUnavailable);
    CHECK(std::string(health->body()) == R"({"status":"draining"})");
    CHECK(health->contentType() == drogon::CT_APPLICATION_JSON);
    CHECK_FALSE(health->getHeader("Content-Security-Policy").empty());
    CHECK(health->getHeader("Cache-Control").empty());
    const auto [versionResult, version] = fetch(server.port, "/version");
    REQUIRE(versionResult == drogon::ReqResult::Ok);
    CHECK(version->statusCode() == drogon::k200OK);

    // Joining goes on, and a connection that comes during the drain is told right after its welcome,
    // with less time than the first.
    std::this_thread::sleep_for(500ms);
    mark = newcomer.messages().size();
    newcomer.send(R"({"type":"joinTable","code":")" + code + R"(","profile":)"
                  R"({"name":"Bea","gender":"female","avatar":"woman-long-red-hair"}})");
    CHECK(newcomer.waitFor([&](const Json::Value& m) { return isType(m, "sync") && m["table"]["code"] == code; }, 5s, mark));

    TestClient latecomer(server.port, "/ws");
    latecomer.send(hello);
    REQUIRE(latecomer.waitForMessages(3, 5s));
    const std::vector<Json::Value> late = latecomer.json();
    CHECK(late[0]["type"] == "welcome");
    REQUIRE(late[1]["type"] == "restarting");
    CHECK(late[2]["type"] == "sync");
    CHECK(late[1]["deadline"].asInt64() < restarting["deadline"].asInt64() - 400);
    CHECK(late[1]["deadline"].asInt64() > 0);

    // At the deadline the game is ended, and the server is gone, with the exit code of a clean stop.
    CHECK(server.process->waitForLine("drain time is up: ending 1 running game(s)", 10s));
    CHECK(server.process->waitForExit(5s) == 0);
    INFO(server.process->log());
    CHECK(since(asked) >= 2900ms);
    CHECK(since(asked) < 8s);
    CHECK(player.waitForClose(5s));
    CHECK(newcomer.waitForClose(5s));
    CHECK(latecomer.waitForClose(5s));
}

TEST_CASE("A second stop signal ends the games at once", "[drain][process]")
{
    Server server(R"(, "drain_ms": 600000)");

    TestClient player(server.port, "/ws");
    greet(player);
    player.send(humanGame);
    REQUIRE(player.waitFor([](const Json::Value& m) { return isType(m, "prompt"); }, 5s));

    server.process->signal(SIGTERM);
    REQUIRE(server.process->waitForLine("stop asked for: no new games, 1 running may finish within 600 s", 5s));

    // Draining, and the game goes on: nothing has ended it after a good while.
    CHECK_FALSE(server.process->waitForLine("shutting down", 500ms));
    CHECK_FALSE(server.process->waitForExit(300ms));

    // Ctrl-C after SIGTERM, or the other way round: either is a second one.
    const auto again = Clock::now();
    server.process->signal(SIGINT);
    CHECK(server.process->waitForLine("stop asked for again: ending 1 running game(s) now", 5s));
    CHECK(server.process->waitForExit(5s) == 0);
    INFO(server.process->log());
    CHECK(since(again) < 4s);
    CHECK(player.waitForClose(5s));
}

TEST_CASE("A drain of no time ends the games the moment the stop is asked for", "[drain][process]")
{
    Server server(R"(, "drain_ms": 0)");

    TestClient player(server.port, "/ws");
    greet(player);
    player.send(humanGame);
    REQUIRE(player.waitFor([](const Json::Value& m) { return isType(m, "prompt"); }, 5s));

    server.process->signal(SIGTERM);
    CHECK(server.process->waitForLine("drain time is up: ending 1 running game(s)", 5s));
    CHECK(server.process->waitForExit(5s) == 0);
    CHECK(player.waitForClose(5s));
}
