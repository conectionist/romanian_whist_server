#ifndef WHIST_TESTS_CORE_RECORDER_H
#define WHIST_TESTS_CORE_RECORDER_H

#include "core/Connection.h"
#include "core/Protocol.h"

#include <json/value.h>

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace whist::test
{

// What the leak oracle knew when it was written: a round's start, with the cards each member may
// see, or a card that has just become public.
struct Mark
{
    std::optional<unsigned int> round;
    std::map<MemberId, std::set<std::string>> visible;
    std::optional<std::string> card;
};

struct Entry
{
    MemberId member = 0;
    std::string text;
    Json::Value message;
    std::optional<Mark> mark;
};

// One ordered log of every message to every member, and of the oracle's marks between them.
class Recorder
{
public:
    // A connection whose messages go into this log. Posting only records, as a real one only
    // queues.
    std::shared_ptr<Connection> connect(MemberId member);

    void mark(Mark mark);

    std::vector<Entry> entries() const;
    std::vector<Json::Value> messagesTo(MemberId member) const;

    // The member's next message after `cursor`, which it advances, or nothing on timeout.
    std::optional<Json::Value> next(MemberId member, std::size_t& cursor, std::chrono::milliseconds timeout);

    // Waits until some message to the member satisfies `match`, and returns it.
    std::optional<Json::Value> waitFor(MemberId member, const std::function<bool(const Json::Value&)>& match,
                                       std::chrono::milliseconds timeout);

private:
    void record(MemberId member, std::string text);

    mutable std::mutex mutex_;
    std::condition_variable changed_;
    std::vector<Entry> entries_;
    std::map<MemberId, std::vector<std::size_t>> byMember_;
};

// Parses one line of JSON, or fails the test.
Json::Value parse(const std::string& text);

// Tests time out far later under sanitizers, which slow everything down.
#if defined(__SANITIZE_THREAD__) || defined(__SANITIZE_ADDRESS__)
inline constexpr int slowdown = 10;
#elif defined(__has_feature)
#if __has_feature(thread_sanitizer) || __has_feature(address_sanitizer)
inline constexpr int slowdown = 10;
#else
inline constexpr int slowdown = 1;
#endif
#else
inline constexpr int slowdown = 1;
#endif

} // namespace whist::test

#endif
