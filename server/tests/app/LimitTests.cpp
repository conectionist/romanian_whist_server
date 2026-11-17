#include "RawSocket.h"
#include "ServerProcess.h"
#include "TestSupport.h"

#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <string>
#include <thread>
#include <vector>

using namespace std::chrono_literals;
using namespace whist::test;

namespace
{

// The real server as a process of its own, with small limits. Behind it, 127.0.0.1 is a proxy
// whose X-Forwarded-For is believed, so a test picks the address it connects from.
struct LimitedServer
{
    TempDir dir;
    std::uint16_t port = freePort();
    std::unique_ptr<ServerProcess> process;

    explicit LimitedServer(const std::string& proxies)
    {
        dir.write("index.html", "<!doctype html><title>test</title>\n");
        dir.write("config.json", R"({"port": )" + std::to_string(port) + R"(, "document_root": ")"
                  + dir.path().string() + R"(", "trusted_proxies": )" + proxies + R"(,
                  "max_connections_per_ip": 2, "tables_per_ip_per_hour": 2,
                  "failed_joins_per_ip": 2, "messages_per_second": 5, "message_burst": 20,
                  "max_running_games": 1})");
        process = std::make_unique<ServerProcess>((dir.path() / "config.json").string());
        const bool listening = process->waitForLine("listening on ", 10s).has_value();
        INFO(process->log());
        REQUIRE(listening);
    }
};

LimitedServer& behindProxy()
{
    static LimitedServer server(R"(["127.0.0.1"])");
    return server;
}

Headers from(const std::string& address)
{
    return {{"X-Forwarded-For", address}};
}

bool isError(const Json::Value& message, const std::string& code, const std::string& detail = {})
{
    return message["type"] == "error" && message["code"] == code && (detail.empty() || message["detail"] == detail);
}

// Says hello, and waits for the sync that follows the welcome.
void greet(TestClient& client)
{
    client.send(R"({"type":"hello","protocol":1,"lang":"en"})");
    REQUIRE(client.waitFor([](const Json::Value& m) { return m["type"] == "sync"; }, 5s));
}

bool refusedAs(TestClient& client, const std::string& code, const std::string& detail = {})
{
    return client.waitFor([&](const Json::Value& m) { return isError(m, code, detail); }, 5s).has_value();
}

const std::string soloGame =
    R"({"type":"createSolo","settings":{"structure":"1-8-1","seats":[)"
    R"({"kind":"human","name":"Ana","gender":"female","avatar":"woman-long-red-hair"},)"
    R"({"kind":"computer","level":"cyborg","name":"Bogdan","gender":"male","avatar":"man-dark-hair-beard"}]}})";

bool tableClosed(const Json::Value& m)
{
    return m["type"] == "tableState" && m["state"] == "closed";
}

} // namespace

TEST_CASE("Connections are counted per client address behind a trusted proxy", "[limits][process]")
{
    LimitedServer& server = behindProxy();

    TestClient first(server.port, "/ws", from("203.0.113.1"));
    TestClient second(server.port, "/ws", from("203.0.113.1"));
    greet(first);
    greet(second);

    TestClient third(server.port, "/ws", from("203.0.113.1"));
    CHECK(refusedAs(third, "rateLimited", "connections"));
    CHECK(third.waitForClose(5s));

    // Another client behind the same proxy, and one that came without the header, are no part of it.
    TestClient other(server.port, "/ws", from("203.0.113.2"));
    greet(other);
    TestClient direct(server.port, "/ws");
    greet(direct);

    // An IPv6 client is counted by its /64.
    TestClient v6a(server.port, "/ws", from("2001:db8:1:2::1"));
    TestClient v6b(server.port, "/ws", from("2001:db8:1:2:ffff::9"));
    TestClient v6c(server.port, "/ws", from("2001:db8:1:2::3"));
    CHECK(refusedAs(v6c, "rateLimited", "connections"));

    // A place comes free when a connection closes.
    first.close();
    REQUIRE(first.waitForClose(5s));
    std::this_thread::sleep_for(200ms);
    TestClient again(server.port, "/ws", from("203.0.113.1"));
    greet(again);
}

TEST_CASE("A peer that is not trusted cannot choose its address", "[limits][process]")
{
    LimitedServer server("[]");

    TestClient first(server.port, "/ws", from("203.0.113.1"));
    TestClient second(server.port, "/ws", from("203.0.113.2"));
    greet(first);
    greet(second);

    // The header says another address, and is not believed: all three are the loopback.
    TestClient third(server.port, "/ws", from("203.0.113.3"));
    CHECK(refusedAs(third, "rateLimited", "connections"));
}

TEST_CASE("The rightmost address no proxy of ours wrote is the client", "[limits][process]")
{
    LimitedServer& server = behindProxy();

    // What nginx sends when the client sent a header of its own: its claim, then the address nginx saw.
    TestClient first(server.port, "/ws", from("198.51.100.200, 203.0.113.20"));
    TestClient second(server.port, "/ws", from("198.51.100.201, 203.0.113.20, 127.0.0.1"));
    greet(first);
    greet(second);

    TestClient third(server.port, "/ws", from("203.0.113.20"));
    CHECK(refusedAs(third, "rateLimited", "connections"));

    // The claim on the left is not the client: no one has used its address.
    TestClient claimed(server.port, "/ws", from("198.51.100.200"));
    greet(claimed);
}

TEST_CASE("A connection over its message rate is told, then closed", "[limits][process]")
{
    LimitedServer& server = behindProxy();

    TestClient client(server.port, "/ws", from("203.0.113.30"));
    greet(client);

    // 40 messages at once, to a limit of 20 in a burst and 5 a second.
    for(int i = 0 ; i < 40 ; i++)
        client.send(R"({"type":"pause"})");
    CHECK(refusedAs(client, "rateLimited"));
    CHECK(client.waitForClose(5s));

    const std::vector<Json::Value> messages = client.json();
    int rateLimited = 0;
    for(const Json::Value& message : messages)
    {
        if(isError(message, "rateLimited"))
        {
            rateLimited++;
            CHECK_FALSE(message.isMember("detail"));
        }
    }
    CHECK(rateLimited == 2);
}

TEST_CASE("Tables made from one address are limited, and a client of another is not", "[limits][process]")
{
    LimitedServer& server = behindProxy();

    TestClient ana(server.port, "/ws", from("203.0.113.40"));
    greet(ana);
    for(int i = 0 ; i < 2 ; i++)
    {
        const std::size_t mark = ana.messages().size();
        ana.send(soloGame);
        REQUIRE(ana.waitFor([](const Json::Value& m) { return m["type"] == "tableState" && m["state"] == "playing"; }, 5s, mark));
        ana.send(R"({"type":"leaveTable"})");
        REQUIRE(ana.waitFor(tableClosed, 5s, mark));
    }

    ana.send(soloGame);
    CHECK(refusedAs(ana, "rateLimited", "tables"));

    TestClient bea(server.port, "/ws", from("203.0.113.41"));
    greet(bea);
    bea.send(soloGame);
    CHECK(bea.waitFor([](const Json::Value& m) { return m["type"] == "tableState"; }, 5s));
    bea.send(R"({"type":"leaveTable"})");
    CHECK(bea.waitFor(tableClosed, 5s));
}

TEST_CASE("Joins that find no table are limited", "[limits][process]")
{
    LimitedServer& server = behindProxy();

    TestClient guesser(server.port, "/ws", from("203.0.113.50"));
    greet(guesser);
    const std::string join = R"({"type":"joinTable","code":"ZZZZZZ","profile":)"
                             R"({"name":"Bea","gender":"female","avatar":"woman-long-red-hair"}})";

    for(int i = 0 ; i < 2 ; i++)
    {
        const std::size_t mark = guesser.messages().size();
        guesser.send(join);
        REQUIRE(guesser.waitFor([](const Json::Value& m) { return isError(m, "notFound"); }, 5s, mark));
    }
    guesser.send(join);
    CHECK(refusedAs(guesser, "rateLimited", "joins"));
}

TEST_CASE("No more games run than max_running_games, and the refusal is busy", "[limits][process]")
{
    LimitedServer server(R"(["127.0.0.1"])");

    TestClient ana(server.port, "/ws", from("203.0.113.60"));
    TestClient bea(server.port, "/ws", from("203.0.113.61"));
    greet(ana);
    greet(bea);

    ana.send(soloGame);
    REQUIRE(ana.waitFor([](const Json::Value& m) { return m["type"] == "prompt"; }, 5s));

    bea.send(soloGame);
    CHECK(refusedAs(bea, "busy"));

    // Her game ends, and its place is free: a game's thread is gone a moment after its table.
    ana.send(R"({"type":"leaveTable"})");
    REQUIRE(ana.waitFor(tableClosed, 5s));
    for(int attempt = 0 ; attempt < 50 ; attempt++)
    {
        const std::size_t mark = bea.messages().size();
        bea.send(soloGame);
        if(bea.waitFor([](const Json::Value& m) { return m["type"] == "prompt"; }, 200ms, mark))
            return;
        std::this_thread::sleep_for(50ms);
    }
    FAIL("the place was never given back");
}

TEST_CASE("Only the first of two X-Forwarded-For lines is read", "[limits][process]")
{
    LimitedServer& server = behindProxy();

    // Drogon keeps the first line of a header sent twice. A proxy that adds its own line, rather than
    // extending the one it was sent, would therefore be read as whatever the client wrote first.
    RawSocket first(server.port, "/ws", "X-Forwarded-For: 203.0.113.70\r\nX-Forwarded-For: 203.0.113.71\r\n");
    RawSocket second(server.port, "/ws", "X-Forwarded-For: 203.0.113.70\r\nX-Forwarded-For: 203.0.113.71\r\n");

    TestClient counted(server.port, "/ws", from("203.0.113.70"));
    CHECK(refusedAs(counted, "rateLimited", "connections"));

    TestClient notCounted(server.port, "/ws", from("203.0.113.71"));
    greet(notCounted);
}
