#include "RawSocket.h"
#include "ServerProcess.h"
#include "TestSupport.h"

#include <catch2/catch_test_macros.hpp>

#include <string>

using namespace std::chrono_literals;
using namespace whist::test;

namespace
{

Json::Value typed(const std::string& type)
{
    Json::Value message(Json::objectValue);
    message["type"] = type;
    return message;
}

std::string profileJson(const std::string& name)
{
    return R"({"name":")" + name + R"(","gender":"female","avatar":"woman-long-red-hair"})";
}

// Three seats: two open and a computer player.
std::string createTableText(const std::string& host)
{
    return std::string(R"({"type":"createTable","settings":{"structure":"8-1-8","foreheadAndHidden":false,)"
                       R"("botSpeed":4,"seats":[{"kind":"open"},{"kind":"open"},{"kind":"computer",)"
                       R"("level":"first-card","name":"Ema","gender":"female","avatar":"woman-long-red-hair"}]},)"
                       R"("profile":)") + profileJson(host) + "}";
}

std::string joinText(const std::string& code, const std::string& name)
{
    return R"({"type":"joinTable","code":")" + code + R"(","profile":)" + profileJson(name) + "}";
}

const std::string helloText = R"({"type":"hello","protocol":1,"lang":"en"})";

bool seatIs(const Json::Value& message, unsigned int seat, const char* presence)
{
    return message["type"] == "tableState" && message["seats"][seat]["presence"] == presence;
}

// Says hello and makes a table; returns its code.
std::string makeTable(TestClient& host)
{
    host.send(helloText);
    REQUIRE(host.waitFor([](const Json::Value& m) { return m["type"] == "sync"; }, 5s));
    host.send(createTableText("Ana"));
    const auto made = host.waitFor([](const Json::Value& m) { return m["type"] == "sync" && m["table"].isObject(); }, 5s);
    REQUIRE(made);
    return host.json()[*made]["table"]["code"].asString();
}

} // namespace

TEST_CASE("A connection that never answers a ping is closed, and the others see its player away",
          "[drops][pong]")
{
    TempDir dir;
    const std::uint16_t port = freePort();
    dir.write("index.html", "<!doctype html><title>test</title>\n");
    dir.write("config.json", R"({"port": )" + std::to_string(port) +
                                 R"(, "ping_interval_ms": 100, "pong_timeout_ms": 700, "reap_interval_ms": 50,)"
                                 R"( "document_root": ")" + dir.path().string() + R"("})");

    ServerProcess server((dir.path() / "config.json").string());
    const bool listening = server.waitForLine("listening on ", 10s).has_value();
    INFO(server.log());
    REQUIRE(listening);

    TestClient ana(port, "/ws");
    const std::string code = makeTable(ana);

    // Bea says hello and joins, then reads nothing, so every ping goes unanswered.
    RawSocket bea(port);
    bea.sendText(helloText);
    bea.sendText(joinText(code, "Bea"));
    REQUIRE(ana.waitFor([](const Json::Value& m) { return seatIs(m, 1, "here"); }, 5s));

    CHECK(ana.waitFor([](const Json::Value& m) { return seatIs(m, 1, "away"); }, 10s));
    CHECK(bea.waitForClose(5s));

    // Ana answers every ping, so hers stays.
    CHECK_FALSE(ana.waitForClose(300ms));
}

TEST_CASE("A player whose tab closes is away, and a new tab with the same token is back in the seat",
          "[drops][reconnect]")
{
    TestClient ana(testServer().port, "/ws");
    const std::string code = makeTable(ana);

    TestClient bea(testServer().port, "/ws");
    bea.send(helloText);
    const auto welcomed = bea.waitFor([](const Json::Value& m) { return m["type"] == "welcome"; }, 5s);
    REQUIRE(welcomed);
    const std::string token = bea.json()[*welcomed]["token"].asString();
    bea.send(joinText(code, "Bea"));
    REQUIRE(ana.waitFor([](const Json::Value& m) { return seatIs(m, 1, "here"); }, 5s));

    bea.close();
    const auto away = ana.waitFor([](const Json::Value& m) { return seatIs(m, 1, "away"); }, 5s);
    REQUIRE(away);

    TestClient again(testServer().port, "/ws");
    Json::Value hello = typed("hello");
    hello["protocol"] = 1;
    hello["lang"] = "en";
    hello["token"] = token;
    again.sendJson(hello);
    const auto sync = again.waitFor([](const Json::Value& m) { return m["type"] == "sync"; }, 5s);
    REQUIRE(sync);
    CHECK(again.json()[*sync]["table"]["seats"][1]["you"] == true);
    CHECK(again.json()[*sync]["table"]["seats"][1]["presence"] == "here");
    CHECK(ana.waitFor([](const Json::Value& m) { return seatIs(m, 1, "here"); }, 5s, *away + 1));
}
