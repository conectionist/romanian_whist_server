#ifndef WHIST_APP_SERVEROPTIONS_H
#define WHIST_APP_SERVEROPTIONS_H

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

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

    // How often the log says what the server is doing: connections, tables, game threads, moves
    // made for members, and the processor's share.
    std::chrono::milliseconds statusLogInterval{60000};

    // How long, from when their connection went, an online player's turn is waited for before a
    // computer player takes their seat.
    std::chrono::milliseconds gracePeriod{30000};

    // How long a table is kept once nobody at it is connected: five minutes online, ten solo.
    std::chrono::milliseconds onlineIdle{5 * 60 * 1000};
    std::chrono::milliseconds soloIdle{10 * 60 * 1000};

    // A tab left open answers pings, so its player is asked whether they are still here: after this
    // many of their turns in a row ran out, or after the game waited this long on them. One who does
    // not answer in time counts as away, as if the connection had closed.
    int stillHereAfterTimeouts = 3;
    std::chrono::milliseconds stillHereAfter{5 * 60 * 1000};
    std::chrono::milliseconds stillHereAnswer{30000};

    // How long a member who is at no table and not connected is remembered.
    std::chrono::milliseconds forgetMember{60 * 60 * 1000};

    // How long, once the server is asked to stop, the games that someone is at may go on before they
    // are ended. The server quits sooner when the last of them is over, or nobody is left at them.
    std::chrono::milliseconds drainTime{15 * 60 * 1000};

    // The shortest beat a computer player's move gets, even at Instant, so a watched game of
    // computer players does not hold a core for its whole length.
    std::chrono::milliseconds instantFloor{30};
    std::string logLevel = "info";

    // Per connection, a token bucket: this many messages a second on average, bursts up to the
    // second number. Past it a connection is told, and closed if it does it again within 10 s.
    unsigned int messagesPerSecond = 20;
    unsigned int messageBurst = 40;

    // Connections in all, which Drogon enforces, and from one address, which the server does: Drogon
    // counts the TCP peer, and behind a proxy that is the proxy.
    std::size_t maxConnections = 2000;
    unsigned int maxConnectionsPerIp = 16;

    // Tables made from one address in an hour, and joins that found no table from one address within
    // the window, which is how a six-character code would be guessed.
    unsigned int tablesPerIpPerHour = 30;
    unsigned int failedJoinsPerIp = 10;
    std::chrono::milliseconds failedJoinWindow{10 * 60 * 1000};

    // A game someone is at that would be the one past this is refused as busy, and so is a table that
    // would be the one past maxTables, whoever is at the ones there are.
    std::size_t maxRunningGames = 40;
    std::size_t maxTables = 400;

    // Of the games that count against maxRunningGames, the most at Instant speed, past which one is
    // refused as busy too, and of those the most that one address may start, past which it is
    // refused as rate limited.
    std::size_t maxInstantGames = 4;
    std::size_t maxInstantGamesPerIp = 1;

    // Addresses and ranges whose X-Forwarded-For is believed. Never a range the open internet can
    // reach the server from: anyone in it could then write any address they like.
    std::vector<std::string> trustedProxies{"127.0.0.1", "::1"};
};

// Both throw std::invalid_argument naming the key at fault. Keys left out keep their defaults;
// unknown keys are refused, so a misspelt one cannot be silently ignored.
ServerOptions parseOptions(std::string_view json);
ServerOptions loadOptions(const std::string& path);

} // namespace whist

#endif
