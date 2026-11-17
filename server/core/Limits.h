#ifndef WHIST_CORE_LIMITS_H
#define WHIST_CORE_LIMITS_H

#include "core/Protocol.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>

namespace whist
{

// Every number is a limit, and 0 turns it off, so a core test that plays fast is never held back.
// The server's own values come from its configuration.
struct LimitOptions
{
    // A connection's messages: this many a second on average, bursts up to the second number.
    unsigned int messagesPerSecond = 0;
    unsigned int messageBurst = 0;

    // Open connections from one address.
    unsigned int connectionsPerAddress = 0;

    // Tables made from one address in any hour, solo and online together.
    unsigned int tablesPerAddressPerHour = 0;

    // Joins that found no table, from one address, within the window.
    unsigned int failedJoinsPerAddress = 0;
    std::chrono::milliseconds failedJoinWindow{10 * 60 * 1000};

    // The time every window reads, so a test moves it by hand. Empty: the steady clock.
    std::function<Clock::time_point()> clock;
};

// A connection's allowance of messages. Not thread safe: a connection is served one message at a time.
class TokenBucket
{
public:
    // Starts full. `perSecond` 0 never refuses.
    TokenBucket(unsigned int perSecond, unsigned int burst, Clock::time_point now);

    // Takes a token if there is one.
    bool take(Clock::time_point now);

private:
    // Counted in millionths of a token, so that a rate of 20 a second gives a token at exactly 50 ms.
    std::int64_t perMicrosecond_;
    std::int64_t capacity_;
    std::int64_t tokens_;
    Clock::time_point last_;
};

// What the server holds against one address: connections, tables made and joins that failed. An
// address is the key clientKey() gives, and an empty one is not limited, as in a core test.
class Limits
{
public:
    explicit Limits(LimitOptions options = {});

    Clock::time_point now() const;
    TokenBucket messageBucket() const;

    // Another connection from the address, or false when it has too many. Each true is paid back
    // with one releaseConnection().
    bool admitConnection(const std::string& address);
    void releaseConnection(const std::string& address);

    // A table made from the address, or false when it has made too many this hour. Paid back
    // when the table was refused after all.
    bool takeTable(const std::string& address);
    void refundTable(const std::string& address);

    // Whether the address has failed to join too often to be let try again.
    bool joinsBlocked(const std::string& address);
    void noteFailedJoin(const std::string& address);

    // How many addresses are remembered, for tests.
    std::size_t remembered() const;

    static constexpr std::chrono::hours tableWindow{1};

private:
    using Times = std::deque<Clock::time_point>;
    using History = std::unordered_map<std::string, Times>;

    static void expire(Times& times, Clock::time_point now, std::chrono::milliseconds window);
    void forgetOldLocked(Clock::time_point now);

    const LimitOptions options_;

    mutable std::mutex mutex_;
    std::unordered_map<std::string, unsigned int> connections_;
    History tables_;
    History failedJoins_;
    Clock::time_point swept_;
};

} // namespace whist

#endif
