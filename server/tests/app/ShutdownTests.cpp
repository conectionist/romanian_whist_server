#include "TestSupport.h"

#include <catch2/catch_test_macros.hpp>

#include <csignal>
#include <optional>
#include <string>
#include <thread>

#include <poll.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

using namespace std::chrono_literals;
using namespace whist::test;

namespace
{

// whist_server as a child process, with its standard output read through a pipe.
class ServerProcess
{
public:
    explicit ServerProcess(const std::string& configPath)
    {
        int fds[2];
        REQUIRE(::pipe(fds) == 0);

        posix_spawn_file_actions_t actions;
        posix_spawn_file_actions_init(&actions);
        posix_spawn_file_actions_adddup2(&actions, fds[1], STDOUT_FILENO);
        posix_spawn_file_actions_addclose(&actions, fds[0]);
        posix_spawn_file_actions_addclose(&actions, fds[1]);

        std::string path = WHIST_SERVER_PATH;
        std::string flag = "--config";
        std::string config = configPath;
        char* argv[] = {path.data(), flag.data(), config.data(), nullptr};
        const int spawned = posix_spawn(&pid_, path.c_str(), &actions, nullptr, argv, environ);
        posix_spawn_file_actions_destroy(&actions);
        ::close(fds[1]);
        output_ = fds[0];
        REQUIRE(spawned == 0);
    }

    ~ServerProcess()
    {
        if(pid_ > 0)
        {
            ::kill(pid_, SIGKILL);
            ::waitpid(pid_, nullptr, 0);
        }
        ::close(output_);
    }

    ServerProcess(const ServerProcess&) = delete;
    ServerProcess& operator=(const ServerProcess&) = delete;

    // Reads output until a line contains `text`, and returns the rest of that line after it.
    std::optional<std::string> waitForLine(const std::string& text, std::chrono::milliseconds timeout)
    {
        const auto end = std::chrono::steady_clock::now() + timeout;
        for(;;)
        {
            for(std::size_t lineEnd ; (lineEnd = pending_.find('\n')) != std::string::npos ; )
            {
                const std::string line = pending_.substr(0, lineEnd);
                pending_.erase(0, lineEnd + 1);
                log_ += line + "\n";
                if(const std::size_t at = line.find(text) ; at != std::string::npos)
                    return line.substr(at + text.size());
            }

            const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(end - std::chrono::steady_clock::now());
            if(left <= 0ms)
                return std::nullopt;

            pollfd readable{output_, POLLIN, 0};
            if(::poll(&readable, 1, static_cast<int>(left.count())) <= 0)
                continue;

            char buffer[4096];
            const ssize_t got = ::read(output_, buffer, sizeof buffer);
            if(got <= 0)
                return std::nullopt;
            pending_.append(buffer, static_cast<std::size_t>(got));
        }
    }

    void signal(int number)
    {
        REQUIRE(::kill(pid_, number) == 0);
    }

    // The exit status, or nothing if the process is still running after the timeout.
    std::optional<int> waitForExit(std::chrono::milliseconds timeout)
    {
        const auto end = std::chrono::steady_clock::now() + timeout;
        while(std::chrono::steady_clock::now() < end)
        {
            int status = 0;
            const pid_t done = ::waitpid(pid_, &status, WNOHANG);
            if(done == pid_)
            {
                pid_ = -1;
                return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
            }
            std::this_thread::sleep_for(20ms);
        }
        return std::nullopt;
    }

    const std::string& log() const { return log_; }

private:
    pid_t pid_ = -1;
    int output_ = -1;
    std::string pending_;
    std::string log_;
};

} // namespace

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
                R"({"kind":"human","name":"Ana","gender":"female","avatar":"face"},)"
                R"({"kind":"computer","level":"cyborg","name":"Bogdan","gender":"male","avatar":"face"}]}})");
    REQUIRE(player.waitFor([](const Json::Value& m) { return m["type"] == "prompt"; }, 5s));

    TestClient watcher(port, "/ws");
    watcher.send(R"({"type":"hello","protocol":1,"lang":"en"})");
    REQUIRE(watcher.waitForMessages(2, 5s));
    watcher.send(R"({"type":"createSolo","settings":{"structure":"1-8-1","botSpeed":0,"seats":[)"
                 R"({"kind":"computer","level":"reckoner-gamma","name":"Ana","gender":"female","avatar":"face"},)"
                 R"({"kind":"computer","level":"cyborg","name":"Bogdan","gender":"male","avatar":"face"}]}})");
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
