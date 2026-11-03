#ifndef WHIST_TESTS_TESTSUPPORT_H
#define WHIST_TESTS_TESTSUPPORT_H

#include <drogon/WebSocketClient.h>
#include <trantor/net/EventLoopThread.h>

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace whist::test
{

// A directory of its own under the system's temporary directory, removed with everything in it.
class TempDir
{
public:
    TempDir();
    ~TempDir();
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;

    const std::filesystem::path& path() const { return path_; }
    void write(const std::string& name, const std::string& contents) const;

private:
    std::filesystem::path path_;
};

// The server main() runs, started in this process on first use, on a free port. Drogon allows one
// app() per process, so every test in the process shares it. Its document root holds an index.html
// containing indexMarker.
struct TestServer
{
    std::uint16_t port = 0;
    std::filesystem::path documentRoot;
};

inline constexpr const char* indexMarker = "whist-test-index";

const TestServer& testServer();

// A port nothing listens on just now. Servers are never given port 0, which would give each of
// their I/O threads a different port.
std::uint16_t freePort();

// A WebSocket client on a loop of its own. The constructor connects or fails the test.
class TestClient
{
public:
    TestClient(std::uint16_t port, const std::string& path);
    ~TestClient();
    TestClient(const TestClient&) = delete;
    TestClient& operator=(const TestClient&) = delete;

    void send(const std::string& text);
    void close();

    // Each waits until its condition holds or the timeout passes, and says which.
    bool waitForMessages(std::size_t count, std::chrono::milliseconds timeout);
    bool waitForClose(std::chrono::milliseconds timeout);

    std::vector<std::string> messages();
    bool sawCloseFrame();

private:
    // Shared with the client's callbacks, which can outlive the constructor and the client: Drogon's
    // client reconnects by itself a second after a drop, and answers the connect callback again.
    struct State
    {
        std::mutex mutex;
        std::condition_variable changed;
        std::optional<drogon::ReqResult> connected;
        std::vector<std::string> messages;
        bool closed = false;
        bool closeFrame = false;
    };

    trantor::EventLoopThread loop_;
    std::shared_ptr<State> state_ = std::make_shared<State>();
    drogon::WebSocketClientPtr client_;
};

} // namespace whist::test

#endif
