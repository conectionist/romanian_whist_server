#include "ServerProcess.h"
#include "TestSupport.h"

#include "app/HealthCheck.h"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <optional>
#include <string>
#include <thread>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

using namespace std::chrono_literals;
using namespace whist;
using namespace whist::test;

namespace
{

std::string configFor(const TempDir& dir, std::uint16_t port, const std::string& extra = {})
{
    dir.write("index.html", "<!doctype html><title>test</title>\n");
    dir.write("config.json", R"({"port": )" + std::to_string(port) + R"(, "document_root": ")" + dir.path().string()
                             + R"(")" + extra + "}");
    return (dir.path() / "config.json").string();
}

// Something else that listens on the port: it takes a connection, reads the request, and answers
// with `reply`, or with nothing at all when `reply` is empty.
class OtherServer
{
public:
    OtherServer(std::uint16_t port, std::string reply) : reply_(std::move(reply))
    {
        listener_ = ::socket(AF_INET, SOCK_STREAM, 0);
        REQUIRE(listener_ >= 0);
        const int on = 1;
        ::setsockopt(listener_, SOL_SOCKET, SO_REUSEADDR, &on, sizeof on);

        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = htons(port);
        REQUIRE(::inet_pton(AF_INET, "127.0.0.1", &address.sin_addr) == 1);
        REQUIRE(::bind(listener_, reinterpret_cast<sockaddr*>(&address), sizeof address) == 0);
        REQUIRE(::listen(listener_, 4) == 0);

        thread_ = std::thread([this]
        {
            while(!stop_)
            {
                pollfd waiting{listener_, POLLIN, 0};
                if(::poll(&waiting, 1, 50) <= 0)
                    continue;
                const int client = ::accept(listener_, nullptr, nullptr);
                if(client < 0)
                    continue;

                char request[1024];
                ::recv(client, request, sizeof request, 0);
                if(!reply_.empty())
                    ::send(client, reply_.data(), reply_.size(), MSG_NOSIGNAL);
                else
                {
                    for(int slice = 0 ; slice < 80 && !stop_ ; slice++)
                        std::this_thread::sleep_for(50ms);
                }
                ::close(client);
            }
        });
    }

    ~OtherServer()
    {
        stop_ = true;
        thread_.join();
        ::close(listener_);
    }

private:
    const std::string reply_;
    int listener_ = -1;
    std::atomic<bool> stop_{false};
    std::thread thread_;
};

} // namespace

TEST_CASE("What /health answered is read as ok, draining or neither", "[health-check]")
{
    const std::string ok = "HTTP/1.1 200 OK\r\ncontent-type: application/json; charset=utf-8\r\n\r\n";
    const std::string no = "HTTP/1.1 503 Service Unavailable\r\ncontent-type: application/json; charset=utf-8\r\n\r\n";

    CHECK(healthOf(ok + R"({"status":"ok"})") == Health::Ok);
    CHECK(healthOf(no + R"({"status":"draining"})") == Health::Draining);
    CHECK(healthOf("HTTP/1.0 200 OK\r\n\r\n" + std::string(R"({"status":"ok"})")) == Health::Ok);

    // A 503 from something that is not a draining whist server, a 200 with another body, and the rest.
    CHECK(healthOf(no + "no healthy upstream") == Health::Bad);
    CHECK(healthOf(no + R"({"status":"ok"})") == Health::Bad);
    CHECK(healthOf(ok + R"({"status":"draining"})") == Health::Bad);
    CHECK(healthOf(ok + "<html>hello</html>") == Health::Bad);
    CHECK(healthOf("HTTP/1.1 404 Not Found\r\n\r\n") == Health::Bad);
    CHECK(healthOf("HTTP/1.1 500 Internal Server Error\r\n\r\n" + std::string(R"({"status":"ok"})")) == Health::Bad);
    CHECK(healthOf("HTTP/1.1 200 OK\r\n") == Health::Bad);
    CHECK(healthOf("") == Health::Bad);
    CHECK(healthOf("garbage") == Health::Bad);
}

TEST_CASE("A server that runs is healthy, asked on the address it listens on or at the loopback", "[health-check][process]")
{
    for(const char* address : {"127.0.0.1", "0.0.0.0"})
    {
        CAPTURE(address);
        TempDir dir;
        const std::uint16_t port = freePort();
        const std::string config = configFor(dir, port, std::string(R"(, "address": ")") + address + R"(")");
        ServerProcess server(config);
        REQUIRE(server.waitForLine("listening on ", 10s));

        const RunResult result = runServer({"--health-check", "--config", config});
        CHECK(result.status == 0);
        CHECK(result.output == "ok\n");
    }
}

TEST_CASE("A server that is draining is still alive", "[health-check][process]")
{
    TempDir dir;
    const std::uint16_t port = freePort();
    const std::string config = configFor(dir, port, R"(, "drain_ms": 60000)");
    ServerProcess server(config);
    REQUIRE(server.waitForLine("listening on ", 10s));

    // A game that waits for its human keeps the drain going.
    TestClient player(port, "/ws");
    player.send(R"({"type":"hello","protocol":2,"lang":"en"})");
    REQUIRE(player.waitForMessages(2, 5s));
    player.send(R"({"type":"createSolo","settings":{"structure":"1-8-1","seats":[)"
                R"({"kind":"human","name":"Ana","gender":"female","avatar":"woman-long-red-hair"},)"
                R"({"kind":"computer","level":"cyborg","name":"Bogdan","gender":"male","avatar":"man-dark-hair-beard"}]}})");
    REQUIRE(player.waitFor([](const Json::Value& m) { return m["type"] == "prompt"; }, 5s));

    server.signal(SIGTERM);
    REQUIRE(server.waitForLine("stop asked for", 5s));

    const RunResult result = runServer({"--health-check", "--config", config});
    CHECK(result.status == 0);
    CHECK(result.output == "draining\n");

    server.signal(SIGINT);
    CHECK(server.waitForExit(5s) == 0);
}

TEST_CASE("Nothing listening is unhealthy, and says so at once", "[health-check][process]")
{
    TempDir dir;
    const std::string config = configFor(dir, freePort());

    const auto started = std::chrono::steady_clock::now();
    const RunResult result = runServer({"--health-check", "--config", config});
    CHECK(result.status == 1);
    CHECK(result.output.starts_with("unhealthy: cannot connect to 127.0.0.1:"));
    CHECK(std::chrono::steady_clock::now() - started < 2s);
}

TEST_CASE("Another server on the port is unhealthy, whatever it answers", "[health-check][process]")
{
    TempDir dir;
    const std::uint16_t port = freePort();
    const std::string config = configFor(dir, port);

    {
        OtherServer other(port, "HTTP/1.1 200 OK\r\nContent-Length: 5\r\nConnection: close\r\n\r\nhello");
        const RunResult result = runServer({"--health-check", "--config", config});
        CHECK(result.status == 1);
        CHECK(result.output == "unhealthy: GET /health was answered \"HTTP/1.1 200 OK\"\n");
    }
    {
        OtherServer other(port, "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
        const RunResult result = runServer({"--health-check", "--config", config});
        CHECK(result.status == 1);
        CHECK(result.output == "unhealthy: GET /health was answered \"HTTP/1.1 404 Not Found\"\n");
    }
}

TEST_CASE("A server that does not answer is unhealthy before the runtime gives up on the check", "[health-check][process]")
{
    TempDir dir;
    const std::uint16_t port = freePort();
    const std::string config = configFor(dir, port);
    OtherServer silent(port, "");

    const auto started = std::chrono::steady_clock::now();
    const RunResult result = runServer({"--health-check", "--config", config});
    CHECK(result.status == 1);
    CHECK(result.output == "unhealthy: no answer in time\n");

    // The runtime's own timeout is 3 s.
    CHECK(std::chrono::steady_clock::now() - started < 3s);
}

TEST_CASE("The health check refuses what it does not understand with 1, which Docker reads as unhealthy, never 2", "[health-check][process]")
{
    // The flag may stand before or after the one it does not know, and the config may be bad.
    CHECK(runServer({"--health-check", "--frobnicate"}).status == 1);
    CHECK(runServer({"--frobnicate", "--health-check"}).status == 1);
    CHECK(runServer({"--health-check", "--config"}).status == 1);

    TempDir dir;
    dir.write("config.json", R"({"port": "http"})");
    const RunResult bad = runServer({"--health-check", "--config", (dir.path() / "config.json").string()});
    CHECK(bad.status == 1);

    CHECK(runServer({"--health-check", "--config", (dir.path() / "missing.json").string()}).status == 1);
}

TEST_CASE("The server itself still refuses a bad flag or configuration with 2", "[health-check][process]")
{
    CHECK(runServer({"--frobnicate"}).status == 2);

    TempDir dir;
    dir.write("config.json", R"({"port": "http"})");
    CHECK(runServer({"--config", (dir.path() / "config.json").string()}).status == 2);
}
