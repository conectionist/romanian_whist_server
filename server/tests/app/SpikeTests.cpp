#include "SpikeController.h"
#include "TestSupport.h"

#include <catch2/catch_test_macros.hpp>

#include <thread>

using namespace std::chrono_literals;
using namespace whist::test;

namespace
{

// Waits for a counter kept by the spike controller to reach a value.
template <typename Read>
bool eventually(Read read, int atLeast, std::chrono::milliseconds timeout)
{
    const auto end = std::chrono::steady_clock::now() + timeout;
    while(read() < atLeast)
    {
        if(std::chrono::steady_clock::now() >= end)
            return false;
        std::this_thread::sleep_for(10ms);
    }
    return true;
}

} // namespace

TEST_CASE("Threads of the server's own can send on a connection at once", "[spike][thread]")
{
    TestClient client(testServer().port, "/spike");
    spike().reset();

    client.send("burst");
    REQUIRE(client.waitForMessages(1000, 10s));
    spike().joinSenders();

    int nextA = 0;
    int nextB = 0;
    for(const std::string& message : client.messages())
    {
        if(message.starts_with("a:"))
            CHECK(message == "a:" + std::to_string(nextA++));
        else if(message.starts_with("b:"))
            CHECK(message == "b:" + std::to_string(nextB++));
        else
            FAIL("unexpected message: " << message);
    }
    CHECK(nextA == 500);
    CHECK(nextB == 500);
}

TEST_CASE("Sending on a connection the client has closed is harmless", "[spike][thread]")
{
    TestClient client(testServer().port, "/spike");
    spike().reset();

    client.send("late");
    REQUIRE(client.waitForMessages(10, 5s));
    client.close();

    REQUIRE(eventually([] { return spike().closedConnections(); }, 1, 5s));
    REQUIRE(eventually([] { return spike().sendsAfterClose(); }, 10, 5s));
    spike().joinSenders();
}

TEST_CASE("Pongs reach handleNewMessage", "[spike][pong]")
{
    TestClient client(testServer().port, "/spike");
    spike().reset();

    // The spike pings every 100 ms.
    CHECK(eventually([] { return spike().pongs(); }, 3, 2s));
}

TEST_CASE("A message over the size cap ends the connection", "[spike][size]")
{
    TestClient client(testServer().port, "/spike");
    spike().reset();

    const std::string atCap(4096, 'x');
    client.send(atCap);
    REQUIRE(client.waitForMessages(1, 5s));
    CHECK(client.messages().front() == atCap);

    client.send(std::string(4097, 'y'));
    CHECK(client.waitForClose(5s));
    CHECK(eventually([] { return spike().closedConnections(); }, 1, 5s));
    CHECK(client.messages().size() == 1);

    // Drogon ends the TCP connection without a Close frame, which a browser reports as code 1006.
    CHECK_FALSE(client.sawCloseFrame());
}
