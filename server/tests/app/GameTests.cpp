#include "TestSupport.h"

#include <catch2/catch_test_macros.hpp>

#include <set>

using namespace std::chrono_literals;
using namespace whist::test;

namespace
{

using Match = std::function<bool(const Json::Value&)>;

Match ofType(std::string type)
{
    return [type](const Json::Value& m) { return m["type"] == type; };
}

Match gameEvent(std::string kind)
{
    return [kind](const Json::Value& m) { return m["type"] == "game" && m["event"]["kind"] == kind; };
}

Json::Value typed(const std::string& type)
{
    Json::Value message(Json::objectValue);
    message["type"] = type;
    return message;
}

Json::Value hello(const std::optional<std::string>& token = std::nullopt)
{
    Json::Value message = typed("hello");
    message["protocol"] = 1;
    message["lang"] = "en";
    if(token)
        message["token"] = *token;
    return message;
}

// `seats` seats of Cyborgs at Instant, with a human at `human` when there is one.
Json::Value createSolo(unsigned int seats, std::optional<unsigned int> human)
{
    static const char* const names[] = {"Ana", "Bogdan", "Cora", "Dan", "Ema", "Filip"};
    Json::Value settings(Json::objectValue);
    for(unsigned int i = 0 ; i < seats ; i++)
    {
        Json::Value seat(Json::objectValue);
        seat["kind"] = human == i ? "human" : "computer";
        if(human != i)
            seat["level"] = "cyborg";
        seat["name"] = names[i];
        seat["gender"] = i % 2 == 0 ? "female" : "male";
        seat["avatar"] = i % 2 == 0 ? "woman-long-red-hair" : "man-dark-hair-beard";
        settings["seats"].append(seat);
    }
    settings["structure"] = "1-8-1";
    settings["botSpeed"] = 4;

    Json::Value message = typed("createSolo");
    message["settings"] = settings;
    return message;
}

Json::Value answer(const Json::Value& prompt)
{
    const bool bet = prompt["kind"] == "bet";
    Json::Value message = typed(bet ? "bet" : "play");
    message["prompt"] = prompt["id"];
    if(bet)
        message["value"] = prompt["forbidden"] == 0 ? 1 : 0;
    else
        message["index"] = prompt["legal"][0];
    return message;
}

// Says hello and returns the token, once the sync that follows the welcome is in.
std::string greet(TestClient& client, const std::optional<std::string>& token = std::nullopt)
{
    client.sendJson(hello(token));
    REQUIRE(client.waitFor(ofType("sync"), 5s));
    return client.json().front()["token"].asString();
}

// Answers every prompt from `from` on until the game is over, or until `stopAt` says to stop at a
// prompt, which is then left unanswered and returned.
std::optional<Json::Value> play(TestClient& client, std::size_t from, const Match& stopAt = {})
{
    std::set<Json::UInt64> answered;
    while(true)
    {
        const auto at = client.waitFor([](const Json::Value& m)
        {
            return m["type"] == "prompt" || (m["type"] == "game" && m["event"]["kind"] == "gameOver");
        }, 30s, from);
        REQUIRE(at);
        const Json::Value message = client.json()[*at];
        from = *at + 1;
        if(message["type"] == "game")
            return std::nullopt;
        if(answered.contains(message["id"].asUInt64()))
            continue;
        if(stopAt && stopAt(message))
            return message;
        answered.insert(message["id"].asUInt64());
        client.sendJson(answer(message));
    }
}

} // namespace

TEST_CASE("/ws answers hello with a welcome and a sync", "[game]")
{
    TestClient client(testServer().port, "/ws");
    client.sendJson(hello());
    REQUIRE(client.waitForMessages(2, 5s));

    const auto messages = client.json();
    CHECK(messages[0]["type"] == "welcome");
    CHECK(messages[0]["seq"] == 1);
    CHECK(messages[0]["token"].asString().size() == 22);
    CHECK(messages[1]["type"] == "sync");
    CHECK(messages[1]["seq"] == 2);
    CHECK(messages[1]["table"].isNull());
}

TEST_CASE("A watched game of computer players runs to the end", "[game]")
{
    TestClient client(testServer().port, "/ws");
    greet(client);
    client.sendJson(createSolo(4, std::nullopt));
    REQUIRE(client.waitFor(gameEvent("gameOver"), 60s));

    const auto messages = client.json();
    for(std::size_t i = 0 ; i < messages.size() ; i++)
    {
        INFO(i);
        CHECK(messages[i]["seq"].asUInt64() == i + 1);
        CHECK(messages[i]["type"] != "error");
    }
}

TEST_CASE("A solo game is played to the end over the socket", "[game]")
{
    TestClient client(testServer().port, "/ws");
    greet(client);
    client.sendJson(createSolo(3, 0u));
    CHECK_FALSE(play(client, 0));
    CHECK_FALSE(client.waitFor(ofType("error"), 0ms));
}

TEST_CASE("A reload mid-trick resumes the game", "[game][reload]")
{
    std::string token;
    Json::Value stoppedAt;
    {
        TestClient first(testServer().port, "/ws");
        token = greet(first);
        first.sendJson(createSolo(4, 1u));

        // Stops at a card prompt, in the third round or later, with cards already on the table.
        const auto stopped = play(first, 0, [&first](const Json::Value& prompt)
        {
            if(prompt["kind"] != "play")
                return false;
            const auto messages = first.json();
            for(auto m = messages.rbegin() ; m != messages.rend() ; ++m)
            {
                if((*m)["type"] == "game")
                    return (*m)["snapshot"]["round"]["index"] >= 2 && !(*m)["snapshot"]["table"]["cards"].empty();
            }
            return false;
        });
        REQUIRE(stopped);
        stoppedAt = *stopped;
    }

    TestClient second(testServer().port, "/ws");
    CHECK(greet(second, token) == token);
    const auto messages = second.json();
    const auto sync = second.waitFor(ofType("sync"), 0ms);
    REQUIRE(sync);
    const Json::Value& synced = messages[*sync];
    CHECK(synced["table"]["state"] == "playing");
    CHECK_FALSE(synced["snapshot"]["table"]["cards"].empty());
    REQUIRE(synced["prompt"].isObject());
    CHECK(synced["prompt"]["id"] == stoppedAt["id"]);

    second.sendJson(answer(synced["prompt"]));
    CHECK_FALSE(play(second, *sync + 1));
}

TEST_CASE("A second tab with the same token takes over, and the first is told why and closed", "[game]")
{
    TestClient first(testServer().port, "/ws");
    const std::string token = greet(first);

    TestClient second(testServer().port, "/ws");
    CHECK(greet(second, token) == token);

    const auto told = first.waitFor([](const Json::Value& m) { return m["code"] == "openElsewhere"; }, 2s);
    REQUIRE(told);
    CHECK(first.waitForClose(900ms));
    CHECK(first.sawCloseFrame());
}
