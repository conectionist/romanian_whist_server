#ifndef WHIST_TESTS_RAWSOCKET_H
#define WHIST_TESTS_RAWSOCKET_H

#include <catch2/catch_test_macros.hpp>

#include <cerrno>
#include <chrono>
#include <cstdint>
#include <string>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

namespace whist::test
{

// A WebSocket client that reads nothing once connected, so it never answers a ping. Drogon's own
// client answers them for itself.
class RawSocket
{
public:
    explicit RawSocket(std::uint16_t port, const std::string& path = "/ws")
    {
        fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        REQUIRE(fd_ >= 0);

        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = htons(port);
        REQUIRE(::inet_pton(AF_INET, "127.0.0.1", &address.sin_addr) == 1);
        REQUIRE(::connect(fd_, reinterpret_cast<sockaddr*>(&address), sizeof address) == 0);

        writeAll("GET " + path + " HTTP/1.1\r\nHost: 127.0.0.1:" + std::to_string(port) +
                 "\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                 "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n\r\n");

        std::string response;
        while(response.find("\r\n\r\n") == std::string::npos)
        {
            char buffer[512];
            const ssize_t got = ::recv(fd_, buffer, sizeof buffer, 0);
            REQUIRE(got > 0);
            response.append(buffer, static_cast<std::size_t>(got));
        }
        REQUIRE(response.starts_with("HTTP/1.1 101"));
    }

    ~RawSocket()
    {
        ::close(fd_);
    }

    RawSocket(const RawSocket&) = delete;
    RawSocket& operator=(const RawSocket&) = delete;

    // One masked text frame, as a browser sends it.
    void sendText(const std::string& text)
    {
        const unsigned char mask[4] = {0x12, 0x34, 0x56, 0x78};
        std::string frame;
        frame += static_cast<char>(0x81);
        if(text.size() < 126)
            frame += static_cast<char>(0x80 | text.size());
        else
        {
            REQUIRE(text.size() < 65536);
            frame += static_cast<char>(0x80 | 126);
            frame += static_cast<char>(text.size() >> 8);
            frame += static_cast<char>(text.size() & 0xFF);
        }
        frame.append(reinterpret_cast<const char*>(mask), 4);
        for(std::size_t i = 0 ; i < text.size() ; i++)
            frame += static_cast<char>(static_cast<unsigned char>(text[i]) ^ mask[i % 4]);
        writeAll(frame);
    }

    // Reads and throws away whatever the server sends, until it closes the connection.
    bool waitForClose(std::chrono::milliseconds timeout)
    {
        const auto end = std::chrono::steady_clock::now() + timeout;
        while(std::chrono::steady_clock::now() < end)
        {
            pollfd readable{fd_, POLLIN, 0};
            if(::poll(&readable, 1, 50) <= 0)
                continue;

            char buffer[4096];
            const ssize_t got = ::recv(fd_, buffer, sizeof buffer, 0);
            if(got == 0 || (got < 0 && errno != EINTR && errno != EAGAIN))
                return true;
        }
        return false;
    }

private:
    void writeAll(const std::string& data)
    {
        std::size_t done = 0;
        while(done < data.size())
        {
            const ssize_t put = ::send(fd_, data.data() + done, data.size() - done, MSG_NOSIGNAL);
            REQUIRE(put > 0);
            done += static_cast<std::size_t>(put);
        }
    }

    int fd_ = -1;
};

} // namespace whist::test

#endif
