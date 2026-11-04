#include "TestSupport.h"

#include <catch2/catch_test_macros.hpp>

#include <drogon/HttpClient.h>

#include <atomic>
#include <set>
#include <thread>

using namespace std::chrono_literals;
using namespace whist::test;

namespace
{

Json::Value typed(const std::string& type)
{
    Json::Value message(Json::objectValue);
    message["type"] = type;
    return message;
}

Json::Value profile(const std::string& name, const std::string& gender)
{
    Json::Value value(Json::objectValue);
    value["name"] = name;
    value["gender"] = gender;
    value["avatar"] = gender == "female" ? "woman-long-red-hair" : "man-dark-hair-beard";
    return value;
}

// Says hello, and waits for the sync that follows the welcome.
void greet(TestClient& client)
{
    Json::Value hello = typed("hello");
    hello["protocol"] = 1;
    hello["lang"] = "en";
    client.sendJson(hello);
    REQUIRE(client.waitFor([](const Json::Value& m) { return m["type"] == "sync"; }, 5s));
}

// Six seats: four open, two computer players. Instant, with a hold after every round.
Json::Value createTable()
{
    Json::Value settings(Json::objectValue);
    for(int i = 0 ; i < 4 ; i++)
        settings["seats"][i]["kind"] = "open";
    for(int i = 4 ; i < 6 ; i++)
    {
        Json::Value seat = profile(i == 4 ? "Ema" : "Filip", i == 4 ? "female" : "male");
        seat["kind"] = "computer";
        seat["level"] = "first-card";
        settings["seats"][i] = seat;
    }
    settings["structure"] = "8-1-8";
    settings["foreheadAndHidden"] = false;
    settings["botSpeed"] = 4;
    settings["waitAfterRound"] = true;

    Json::Value message = typed("createTable");
    message["settings"] = settings;
    message["profile"] = profile("Ana", "female");
    return message;
}

struct Played
{
    bool over = false;
    std::size_t holds = 0;
    std::size_t errors = 0;
};

// Answers every prompt with its first legal move, and continues every hold that awaits `seat`, until
// the game is over. Run on a thread of its own, so it asserts nothing.
Played play(TestClient& client, unsigned int seat)
{
    Played played;
    std::set<Json::UInt64> answered;
    std::set<unsigned int> continued;
    std::size_t from = 0;
    while(true)
    {
        const auto at = client.waitFor([](const Json::Value& m)
        {
            return m["type"] == "prompt" || m["type"] == "held" || m["type"] == "error" ||
                   (m["type"] == "game" && m["event"]["kind"] == "gameOver");
        }, 60s, from);
        if(!at)
            return played;

        const Json::Value message = client.json()[*at];
        from = *at + 1;
        if(message["type"] == "game")
        {
            played.over = true;
            return played;
        }
        if(message["type"] == "error")
        {
            played.errors++;
            continue;
        }
        if(message["type"] == "held")
        {
            const unsigned int round = message["round"].asUInt();
            bool awaited = false;
            for(const Json::Value& waiting : message["waitingFor"])
                awaited = awaited || waiting.asUInt() == seat;
            if(awaited && continued.insert(round).second)
            {
                played.holds++;
                Json::Value next = typed("continue");
                next["round"] = round;
                client.sendJson(next);
            }
            continue;
        }
        if(!answered.insert(message["id"].asUInt64()).second)
            continue;

        const bool bet = message["kind"] == "bet";
        Json::Value move = typed(bet ? "bet" : "play");
        move["prompt"] = message["id"];
        if(bet)
            move["value"] = message["forbidden"] == 0 ? 1 : 0;
        else
            move["index"] = message["legal"][0];
        client.sendJson(move);
    }
}

} // namespace

TEST_CASE("Three people and three computer players finish an online game over the socket", "[online]")
{
    const std::uint16_t port = testServer().port;
    TestClient ana(port, "/ws");
    TestClient bogdan(port, "/ws");
    TestClient cora(port, "/ws");
    greet(ana);
    greet(bogdan);
    greet(cora);

    const std::size_t before = ana.json().size();
    ana.sendJson(createTable());
    const auto made = ana.waitFor([](const Json::Value& m) { return m["type"] == "sync"; }, 5s, before);
    REQUIRE(made);
    const std::string code = ana.json()[*made]["table"]["code"].asString();
    REQUIRE(code.size() == 6);

    for(auto [client, name, gender] : {std::tuple{&bogdan, "Bogdan", "male"}, std::tuple{&cora, "Cora", "female"}})
    {
        const std::size_t from = client->json().size();
        Json::Value join = typed("joinTable");
        join["code"] = code;
        join["profile"] = profile(name, gender);
        client->sendJson(join);
        REQUIRE(client->waitFor([](const Json::Value& m) { return m["type"] == "sync"; }, 5s, from));
    }

    // The host fills the last open seat with a computer player, and starts.
    Json::Value seat = typed("setSeat");
    seat["seat"] = 3;
    seat["kind"] = "computer";
    seat["level"] = "reckoner-alpha";
    ana.sendJson(seat);
    const auto filled = ana.waitFor([](const Json::Value& m)
    {
        return m["type"] == "tableState" && m["seats"][3]["kind"] == "computer";
    }, 5s);
    REQUIRE(filled);
    CHECK(ana.json()[*filled]["seats"][3]["level"] == "reckoner-alpha");

    std::atomic<int> started{0};
    Played results[3];
    std::thread players[3];
    TestClient* clients[3] = {&ana, &bogdan, &cora};
    for(unsigned int i = 0 ; i < 3 ; i++)
        players[i] = std::thread([&, i] { started++; results[i] = play(*clients[i], i); });
    while(started < 3)
        std::this_thread::yield();
    ana.sendJson(typed("startGame"));

    for(std::thread& player : players)
        player.join();
    for(unsigned int i = 0 ; i < 3 ; i++)
    {
        INFO("seat " << i);
        CHECK(results[i].over);
        CHECK(results[i].errors == 0);
        CHECK(results[i].holds > 0);
    }

    for(TestClient* client : clients)
    {
        const auto messages = client->json();
        for(std::size_t i = 0 ; i < messages.size() ; i++)
            CHECK(messages[i]["seq"].asUInt64() == i + 1);
    }
}

TEST_CASE("A table's link serves the page", "[static][online]")
{
    const TestServer& server = testServer();

    trantor::EventLoopThread loop;
    loop.run();
    const drogon::HttpClientPtr client = drogon::HttpClient::newHttpClient("127.0.0.1", server.port, false, loop.getLoop());

    const auto get = [&](const std::string& path)
    {
        drogon::HttpRequestPtr request = drogon::HttpRequest::newHttpRequest();
        request->setPath(path);
        return client->sendRequest(request, 5.0);
    };

    const auto [result, response] = get("/t/K7QM4P");
    REQUIRE(result == drogon::ReqResult::Ok);
    REQUIRE(response);
    CHECK(response->statusCode() == drogon::k200OK);
    CHECK(response->contentType() == drogon::CT_TEXT_HTML);
    CHECK(std::string(response->body()).find(indexMarker) != std::string::npos);

    for(const char* path : {"/t/K7QM4", "/t/k7qm4p", "/t/K7QM4P/x"})
    {
        INFO(path);
        const auto [badResult, badResponse] = get(path);
        REQUIRE(badResult == drogon::ReqResult::Ok);
        REQUIRE(badResponse);
        CHECK(badResponse->statusCode() == drogon::k404NotFound);
    }
}
