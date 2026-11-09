#include "ServerProcess.h"
#include "TestSupport.h"

#include <catch2/catch_test_macros.hpp>

#include <optional>
#include <string>
#include <thread>

using namespace std::chrono_literals;
using namespace whist::test;

TEST_CASE("SIGTERM stops the server after the shutdown delay, serving until then", "[spike][sigterm]")
{
    TempDir dir;
    const std::uint16_t port = freePort();
    dir.write("index.html", "<!doctype html><title>test</title>\n");
    dir.write("config.json", R"({"port": )" + std::to_string(port) + R"(, "shutdown_delay_ms": 1000, "document_root": ")"
                             + dir.path().string() + R"("})");

    ServerProcess server((dir.path() / "config.json").string());
    const bool listening = server.waitForLine("listening on ", 10s).has_value();
    INFO(server.log());
    REQUIRE(listening);

    // Two games running when the signal comes: one parked on its human, one of computer players.
    TestClient player(port, "/ws");
    player.send(R"({"type":"hello","protocol":1,"lang":"en"})");
    REQUIRE(player.waitForMessages(2, 5s));
    player.send(R"({"type":"createSolo","settings":{"structure":"1-8-1","seats":[)"
                R"({"kind":"human","name":"Ana","gender":"female","avatar":"woman-long-red-hair"},)"
                R"({"kind":"computer","level":"cyborg","name":"Bogdan","gender":"male","avatar":"man-dark-hair-beard"}]}})");
    REQUIRE(player.waitFor([](const Json::Value& m) { return m["type"] == "prompt"; }, 5s));

    TestClient watcher(port, "/ws");
    watcher.send(R"({"type":"hello","protocol":1,"lang":"en"})");
    REQUIRE(watcher.waitForMessages(2, 5s));
    watcher.send(R"({"type":"createSolo","settings":{"structure":"1-8-1","botSpeed":0,"seats":[)"
                 R"({"kind":"computer","level":"reckoner-gamma","name":"Ana","gender":"female","avatar":"woman-long-red-hair"},)"
                 R"({"kind":"computer","level":"cyborg","name":"Bogdan","gender":"male","avatar":"man-dark-hair-beard"}]}})");
    REQUIRE(watcher.waitFor([](const Json::Value& m) { return m["type"] == "game"; }, 5s));

    server.signal(SIGTERM);
    REQUIRE(server.waitForLine("shutting down", 2s));

    // Still answering during the delay.
    const std::size_t before = player.messages().size();
    player.send("during");
    CHECK(player.waitFor([](const Json::Value& m) { return m["code"] == "badMessage"; }, 500ms, before));

    CHECK(server.waitForExit(5s) == 0);
    CHECK(player.waitForClose(5s));
    CHECK(watcher.waitForClose(5s));
}
