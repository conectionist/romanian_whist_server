#ifndef WHIST_APP_SERVEROPTIONS_H
#define WHIST_APP_SERVEROPTIONS_H

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace whist
{

struct ServerOptions
{
    std::string address = "127.0.0.1";
    std::uint16_t port = 8080;
    std::string documentRoot = "server/www";
    std::size_t ioThreads = 2;
    std::size_t maxMessageBytes = 4096;
    std::chrono::milliseconds pingInterval{10000};

    // A connection nothing is heard from for this long is closed, and its player counts as away. It
    // must be longer than the ping interval.
    std::chrono::milliseconds pongTimeout{30000};

    // How often the server looks for silent connections, and for tables to reap.
    std::chrono::milliseconds reapInterval{1000};

    // How long, from when their connection went, an online player's turn is waited for before a
    // computer player takes their seat.
    std::chrono::milliseconds gracePeriod{30000};

    // How long a table is kept once nobody at it is connected: ten minutes online, half an hour solo.
    std::chrono::milliseconds onlineIdle{10 * 60 * 1000};
    std::chrono::milliseconds soloIdle{30 * 60 * 1000};

    // How long a member who is at no table and not connected is remembered.
    std::chrono::milliseconds forgetMember{60 * 60 * 1000};
    std::chrono::milliseconds shutdownDelay{0};

    // The shortest beat a computer player's move gets, even at Instant, so a watched game of
    // computer players does not hold a core for its whole length.
    std::chrono::milliseconds instantFloor{30};
    std::string logLevel = "info";
};

// Both throw std::invalid_argument naming the key at fault. Keys left out keep their defaults;
// unknown keys are refused, so a misspelt one cannot be silently ignored.
ServerOptions parseOptions(std::string_view json);
ServerOptions loadOptions(const std::string& path);

} // namespace whist

#endif
