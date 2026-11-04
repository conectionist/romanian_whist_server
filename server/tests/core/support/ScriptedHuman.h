#ifndef WHIST_TESTS_CORE_SCRIPTEDHUMAN_H
#define WHIST_TESTS_CORE_SCRIPTEDHUMAN_H

#include "core/Protocol.h"
#include "support/Recorder.h"

#include <json/value.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace whist::test
{

enum class Behaviour
{
    // Answers every prompt with a legal move.
    Good,

    // Before each legal answer, sends a stale one and an illegal one, and after it the same answer
    // again.
    Garbage,

    // Never answers.
    Silent
};

// A player on a thread of its own, as a browser is: it reads what it is sent and answers through
// the same entry point an I/O thread uses. It never asserts off the test's thread; it writes down
// what went wrong instead.
class ScriptedHuman
{
public:
    using Answerer = std::function<void(const Answer&)>;

    // Called with each new prompt before it is answered, while the game waits for this player.
    // Returning true means it answered the prompt itself.
    using OnPrompt = std::function<bool(const Json::Value&)>;

    // Called with a hold's round, to send the Continue that ends it.
    using Continuer = std::function<void(unsigned int)>;

    ScriptedHuman(Recorder& recorder, MemberId member, Answerer answer, Behaviour behaviour, OnPrompt onPrompt = {},
                  Continuer onHeld = {});
    ~ScriptedHuman();

    ScriptedHuman(const ScriptedHuman&) = delete;
    ScriptedHuman& operator=(const ScriptedHuman&) = delete;

    // Until the game ends for this player: gameOver, gameStopped, or the table closing.
    bool waitUntilOver(std::chrono::milliseconds timeout);

    // Until the first prompt arrives.
    bool waitForPrompt(std::chrono::milliseconds timeout);

    struct Counts
    {
        std::size_t messages = 0;
        std::size_t prompts = 0;
        std::size_t closedByYou = 0;
        std::size_t closedByComputer = 0;
        std::size_t illegal = 0;
        std::size_t stale = 0;
        std::size_t notYourTurn = 0;
        std::size_t holds = 0;
    };

    Counts counts() const;

    // Messages that did not decode, and anything else out of order.
    std::vector<std::string> problems() const;

private:
    void run();
    void handle(const Json::Value& message);
    Answer goodAnswer(const Json::Value& prompt) const;

    Recorder& recorder_;
    const MemberId member_;
    const Answerer answer_;
    const Behaviour behaviour_;
    const OnPrompt onPrompt_;
    const Continuer onHeld_;

    mutable std::mutex mutex_;
    std::condition_variable changed_;
    Counts counts_;
    std::vector<std::string> problems_;
    std::uint64_t lastPrompt_ = 0;
    std::optional<unsigned int> seat_;
    std::optional<unsigned int> lastHeld_;
    std::size_t attempts_ = 0;
    bool over_ = false;
    std::atomic<bool> stop_{false};
    std::thread thread_;
};

} // namespace whist::test

#endif
