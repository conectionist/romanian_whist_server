#include "app/HealthCheck.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>

namespace whist
{

namespace
{

using Clock = std::chrono::steady_clock;

// A health check that takes longer than the runtime waits would only be killed, so it gives up first.
constexpr std::chrono::milliseconds patience{2000};

int millisecondsLeft(Clock::time_point end)
{
    const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(end - Clock::now());
    return static_cast<int>(std::max<long long>(left.count(), 0));
}

// Waits until the socket may be read or written, or the time is up.
bool ready(int fd, short events, Clock::time_point end)
{
    pollfd wanted{fd, events, 0};
    return ::poll(&wanted, 1, millisecondsLeft(end)) > 0;
}

std::string loopbackFor(const std::string& address)
{
    if(address == "0.0.0.0")
        return "127.0.0.1";
    if(address == "::")
        return "::1";
    return address;
}

// The server's answer, read to its end, or what went wrong.
bool fetchHealth(const ServerOptions& options, std::string& answer, std::string& problem)
{
    const std::string host = loopbackFor(options.address);
    const Clock::time_point end = Clock::now() + patience;

    sockaddr_storage address{};
    socklen_t length = 0;
    int family = AF_INET;
    if(::inet_pton(AF_INET, host.c_str(), &reinterpret_cast<sockaddr_in*>(&address)->sin_addr) == 1)
    {
        auto* v4 = reinterpret_cast<sockaddr_in*>(&address);
        v4->sin_family = AF_INET;
        v4->sin_port = htons(options.port);
        length = sizeof(sockaddr_in);
    }
    else if(::inet_pton(AF_INET6, host.c_str(), &reinterpret_cast<sockaddr_in6*>(&address)->sin6_addr) == 1)
    {
        auto* v6 = reinterpret_cast<sockaddr_in6*>(&address);
        v6->sin6_family = AF_INET6;
        v6->sin6_port = htons(options.port);
        length = sizeof(sockaddr_in6);
        family = AF_INET6;
    }
    else
    {
        problem = "the address " + host + " is not an IP address";
        return false;
    }

    const int fd = ::socket(family, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if(fd < 0)
    {
        problem = std::string("no socket: ") + std::strerror(errno);
        return false;
    }

    const auto fail = [&](const std::string& what)
    {
        problem = what;
        ::close(fd);
        return false;
    };

    if(::connect(fd, reinterpret_cast<sockaddr*>(&address), length) < 0 && errno != EINPROGRESS)
        return fail(std::string("cannot connect to ") + host + ":" + std::to_string(options.port) + ": " + std::strerror(errno));
    if(!ready(fd, POLLOUT, end))
        return fail("no connection to " + host + ":" + std::to_string(options.port) + " in time");

    int error = 0;
    socklen_t errorSize = sizeof error;
    ::getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &errorSize);
    if(error != 0)
        return fail(std::string("cannot connect to ") + host + ":" + std::to_string(options.port) + ": " + std::strerror(error));

    const std::string request = "GET /health HTTP/1.1\r\nHost: " + host + "\r\nUser-Agent: whist_server --health-check\r\n"
                                "Connection: close\r\n\r\n";
    if(::send(fd, request.data(), request.size(), MSG_NOSIGNAL) != static_cast<ssize_t>(request.size()))
        return fail("could not send the request");

    // The answer is small and the server closes the connection after it.
    constexpr std::size_t most = 4096;
    char buffer[1024];
    while(answer.size() < most)
    {
        if(!ready(fd, POLLIN, end))
            return fail("no answer in time");
        const ssize_t got = ::recv(fd, buffer, sizeof buffer, 0);
        if(got == 0)
            break;
        if(got < 0)
        {
            if(errno == EAGAIN || errno == EINTR)
                continue;
            return fail(std::string("reading failed: ") + std::strerror(errno));
        }
        answer.append(buffer, static_cast<std::size_t>(got));
    }
    ::close(fd);
    return true;
}

} // namespace

Health healthOf(std::string_view answer)
{
    const std::size_t statusAt = answer.find(' ');
    const std::size_t bodyAt = answer.find("\r\n\r\n");
    if(!answer.starts_with("HTTP/1.") || statusAt == std::string_view::npos || bodyAt == std::string_view::npos)
        return Health::Bad;

    const std::string_view status = answer.substr(statusAt + 1, 3);
    const std::string_view body = answer.substr(bodyAt + 4);

    // The body too, so that another server on the port that happens to answer 200 is not taken for ours.
    if(status == "200" && body == R"({"status":"ok"})")
        return Health::Ok;
    if(status == "503" && body == R"({"status":"draining"})")
        return Health::Draining;
    return Health::Bad;
}

int healthCheck(const ServerOptions& options)
{
    std::string answer;
    std::string problem;
    if(!fetchHealth(options, answer, problem))
    {
        std::printf("unhealthy: %s\n", problem.c_str());
        return 1;
    }

    switch(healthOf(answer))
    {
        case Health::Ok:
            std::printf("ok\n");
            return 0;
        case Health::Draining:
            std::printf("draining\n");
            return 0;
        case Health::Bad:
            break;
    }

    const std::size_t lineEnd = answer.find("\r\n");
    std::printf("unhealthy: GET /health was answered \"%s\"\n", answer.substr(0, lineEnd).c_str());
    return 1;
}

} // namespace whist
