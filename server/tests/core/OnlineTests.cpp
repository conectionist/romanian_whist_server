#include "core/Codec.h"
#include "core/Service.h"

#include "support/Client.h"
#include "support/Games.h"
#include "support/Recorder.h"
#include "support/ScriptedHuman.h"
#include "support/ServerMessages.h"

#include <catch2/catch_test_macros.hpp>

#include <map>
#include <memory>

using namespace whist;
using std::chrono::milliseconds;

namespace
{

const milliseconds patience(60000 * test::slowdown);

// Long enough for a message that is coming to have come.
const milliseconds quiet(300 * test::slowdown);

using Match = std::function<bool(const Json::Value&)>;

Match ofType(std::string type)
{
    return [type](const Json::Value& m) { return m["type"] == type; };
}

Json::Value profile(const std::string& name, const std::string& gender = "female")
{
    Json::Value value(Json::objectValue);
    value["name"] = name;
    value["gender"] = gender;
    value["avatar"] = gender == "female" ? "woman-long-red-hair" : "man-dark-hair-beard";
    return value;
}

// `seats` seats: the first `open` open, the rest computer players of `level`.
Settings tableSettings(unsigned int seats, unsigned int open, Level level = Level::FirstCard)
{
    std::vector<unsigned int> humans;
    for(unsigned int i = 0 ; i < open ; i++)
        humans.push_back(i);
    Settings settings = test::tableSettings(seats, humans, level);
    for(unsigned int i = 0 ; i < open ; i++)
        settings.seats[i] = SeatSetting{};
    for(SeatSetting& seat : settings.seats)
        seat.profile.name = seat.kind == SeatKind::Computer ? "Bot-" + seat.profile.name : seat.profile.name;
    settings.structure = romanian_whist::GameStructure::S_818;
    return settings;
}

// People at an online table, each a browser tab through a session.
struct Online
{
    test::Recorder recorder;
    std::shared_ptr<Service> service;
    std::map<MemberId, std::unique_ptr<test::Client>> tabs;
    std::map<MemberId, std::string> tokens;
    std::string code;

    explicit Online(milliseconds roundHold = milliseconds(30000))
    {
        ServiceOptions options;
        options.registry.table.instantFloor = milliseconds(0);
        options.registry.table.threadNiceness = 0;
        options.registry.table.roundHold = roundHold;
        service = std::make_shared<Service>(std::move(options));
    }

    ~Online()
    {
        service->shutdown();
    }

    test::Client& open(MemberId label)
    {
        tabs[label] = std::make_unique<test::Client>(service, recorder, label);
        tokens[label] = tabs[label]->hello()["token"].asString();
        return *tabs[label];
    }

    void send(MemberId label, const Json::Value& message)
    {
        tabs.at(label)->send(message);
    }

    std::size_t mark(MemberId label) const
    {
        return recorder.messagesTo(label).size();
    }

    // The first message at or after `from` that matches.
    std::optional<Json::Value> waitFrom(MemberId label, std::size_t from, const Match& match)
    {
        std::size_t cursor = from;
        while(const std::optional<Json::Value> message = recorder.next(label, cursor, patience))
        {
            if(match(*message))
                return message;
        }
        return std::nullopt;
    }

    // Sends, and returns the first message after it that matches.
    Json::Value ask(MemberId label, const Json::Value& message, const Match& match)
    {
        const std::size_t from = mark(label);
        send(label, message);
        const std::optional<Json::Value> answer = waitFrom(label, from, match);
        REQUIRE(answer);
        return *answer;
    }

    // Sends, and returns the error it was refused with, or null when none came.
    Json::Value refusal(MemberId label, const Json::Value& message)
    {
        const std::size_t from = mark(label);
        send(label, message);
        std::this_thread::sleep_for(quiet);
        const auto messages = recorder.messagesTo(label);
        for(std::size_t i = from ; i < messages.size() ; i++)
        {
            if(messages[i]["type"] == "error")
                return messages[i];
        }
        return Json::nullValue;
    }

    // The table as the member last saw it, from a tableState or a sync.
    Json::Value table(MemberId label) const
    {
        const auto messages = recorder.messagesTo(label);
        for(auto it = messages.rbegin() ; it != messages.rend() ; ++it)
        {
            if((*it)["type"] == "tableState")
                return *it;
            if((*it)["type"] == "sync")
                return (*it)["table"];
        }
        return Json::nullValue;
    }

    void create(MemberId label, const Settings& settings, const std::string& name)
    {
        Json::Value message = test::typed("createTable");
        message["settings"] = encodeSettings(settings);
        message["profile"] = profile(name);
        const Json::Value sync = ask(label, message, ofType("sync"));
        REQUIRE(sync["table"].isObject());
        code = sync["table"]["code"].asString();
    }

    Json::Value join(const std::string& name, const std::string& gender = "female")
    {
        Json::Value message = test::typed("joinTable");
        message["code"] = code;
        message["profile"] = profile(name, gender);
        return message;
    }

    Json::Value setSeat(unsigned int seat, std::optional<Level> level)
    {
        Json::Value message = test::typed("setSeat");
        message["seat"] = seat;
        message["kind"] = level ? "computer" : "open";
        if(level)
            message["level"] = std::string(levelName(*level));
        return message;
    }

    Json::Value takeSeat(unsigned int seat)
    {
        Json::Value message = test::typed("takeSeat");
        message["seat"] = seat;
        return message;
    }

    Json::Value updateSettings(const Json::Value& settings)
    {
        Json::Value message = test::typed("updateSettings");
        message["settings"] = settings;
        return message;
    }
};

void checkShapes(const std::vector<Json::Value>& messages)
{
    for(const Json::Value& message : messages)
    {
        INFO(message.toStyledString());
        CHECK(test::shapeError(message).empty());
    }
}

} // namespace

TEST_CASE("An online table is made with its maker as host, and joined by its code", "[online]")
{
    Online online;
    online.open(1);
    online.open(2);
    online.open(3);
    online.open(4);

    online.create(1, tableSettings(4, 3), "  Ana  ");
    const Json::Value made = online.table(1);
    CHECK(made["mode"] == "online");
    CHECK(made["state"] == "lobby");
    CHECK(made["host"] == 0);
    CHECK(made["seats"][0]["kind"] == "human");
    CHECK(made["seats"][0]["name"] == "Ana");
    CHECK(made["seats"][0]["you"] == true);
    CHECK(made["seats"][0]["presence"] == "here");
    CHECK(made["seats"][1]["kind"] == "open");
    CHECK(made["settings"]["replacement"] == "reckoner-beta");

    // A code that is no table's, or a solo table's, is not found.
    Json::Value wrong = online.join("Bea");
    wrong["code"] = "ZZZZZZ";
    CHECK(online.refusal(2, wrong)["code"] == "notFound");
    Json::Value solo = test::typed("createSolo");
    solo["settings"] = encodeSettings(test::tableSettings(2, {0}));
    online.ask(4, solo, ofType("tableState"));
    Json::Value soloCode = online.join("Bea");
    soloCode["code"] = online.table(4)["code"];
    CHECK(online.refusal(2, soloCode)["code"] == "notFound");

    // The newcomer gets a sync; everyone else a tableState.
    const std::size_t before = online.mark(1);
    const Json::Value joined = online.ask(2, online.join("Bea"), ofType("sync"));
    CHECK(joined["game"].isNull());
    CHECK(joined["table"]["seats"][1]["name"] == "Bea");
    CHECK(joined["table"]["seats"][1]["you"] == true);
    CHECK(joined["table"]["seats"][0]["you"] == false);
    const auto seen = online.waitFrom(1, before, ofType("tableState"));
    REQUIRE(seen);
    CHECK((*seen)["seats"][1]["name"] == "Bea");

    CHECK(online.refusal(2, online.join("Bea"))["code"] == "illegal");

    // A name already taken gets a number.
    const Json::Value third = online.ask(3, online.join("ANA"), ofType("sync"));
    CHECK(third["table"]["seats"][2]["name"] == "ANA 2");

    online.open(5);
    CHECK(online.refusal(5, online.join("Dana"))["code"] == "tableFull");

    checkShapes(online.recorder.messagesTo(1));
    checkShapes(online.recorder.messagesTo(2));
}

TEST_CASE("Online, the lobby's seats, profiles and settings", "[online]")
{
    Online online;
    online.open(1);
    online.open(2);
    online.create(1, tableSettings(4, 3), "Ana");
    online.ask(2, online.join("Bea"), ofType("sync"));

    // The host alone sets seats, and never one a person holds.
    CHECK(online.refusal(2, online.setSeat(2, Level::Cyborg))["code"] == "notHost");
    CHECK(online.refusal(1, online.setSeat(1, Level::Cyborg))["code"] == "illegal");
    const Json::Value set = online.ask(1, online.setSeat(2, Level::Cyborg), ofType("tableState"));
    CHECK(set["seats"][2]["kind"] == "computer");
    CHECK(set["seats"][2]["level"] == "cyborg");
    CHECK_FALSE(set["seats"][2]["name"].asString().empty());

    // People move to open seats.
    CHECK(online.refusal(2, online.takeSeat(2))["code"] == "illegal");
    CHECK(online.refusal(2, online.takeSeat(5))["code"] == "badMessage");
    online.ask(1, online.setSeat(2, std::nullopt), ofType("tableState"));
    const Json::Value moved = online.ask(2, online.takeSeat(2), ofType("tableState"));
    CHECK(moved["seats"][2]["name"] == "Bea");
    CHECK(moved["seats"][2]["you"] == true);
    CHECK(moved["seats"][1]["kind"] == "open");

    // Profiles: names unique, portraits for the person's gender and kept for no level.
    Json::Value rename = test::typed("updateProfile");
    rename["name"] = "ana";
    rename["gender"] = "female";
    rename["avatar"] = "woman-long-brown-hair";
    CHECK(online.refusal(2, rename)["code"] == "nameTaken");
    rename["name"] = "Bogdan";
    rename["gender"] = "male";
    const Json::Value badPortrait = online.refusal(2, rename);
    CHECK(badPortrait["code"] == "badMessage");
    CHECK(badPortrait["detail"] == "avatar");
    rename["avatar"] = "cyborg-man-eye";
    CHECK(online.refusal(2, rename)["detail"] == "avatar");
    rename["avatar"] = "man-stubble";
    CHECK(online.ask(2, rename, ofType("tableState"))["seats"][2]["name"] == "Bogdan");

    // Settings: only the number of seats is read from them, and never cuts a person off.
    Json::Value settings = online.table(1)["settings"];
    settings["botSpeed"] = 4;
    settings["seats"].append(Json::Value(Json::objectValue));
    settings["seats"][4]["kind"] = "open";
    CHECK(online.refusal(2, online.updateSettings(settings))["code"] == "notHost");
    const Json::Value five = online.ask(1, online.updateSettings(settings), ofType("tableState"));
    CHECK(five["settings"]["botSpeed"] == 4);
    CHECK(five["seats"].size() == 5);
    CHECK(five["seats"][4]["kind"] == "open");
    settings["seats"].resize(2);
    CHECK(online.refusal(1, online.updateSettings(settings))["code"] == "illegal");
    settings["seed"] = 4;
    CHECK(online.refusal(1, online.updateSettings(settings))["detail"] == "seed");

    // Starting needs every seat filled, and refusals never close the connection.
    for(int i = 0 ; i < Session::badMessageLimit + 1 ; i++)
    {
        const Json::Value refused = online.refusal(1, test::typed("startGame"));
        CHECK(refused["code"] == "illegal");
        CHECK(refused["detail"] == "seats");
    }
    CHECK(online.refusal(2, test::typed("startGame"))["code"] == "notHost");
    CHECK_FALSE(online.recorder.closedAfter(1));

    checkShapes(online.recorder.messagesTo(1));
}

TEST_CASE("Online, leaving opens the seat, passes the host on, and the last one out closes the table", "[online]")
{
    Online online;
    online.open(1);
    online.open(2);
    online.open(3);
    online.create(1, tableSettings(3, 3), "Ana");
    online.ask(2, online.join("Bea"), ofType("sync"));
    online.ask(3, online.join("Cora"), ofType("sync"));

    const std::size_t before = online.mark(2);
    const Json::Value left = online.ask(1, test::typed("leaveTable"), ofType("sync"));
    CHECK(left["table"].isNull());
    const auto seen = online.waitFrom(2, before, ofType("tableState"));
    REQUIRE(seen);
    CHECK((*seen)["host"] == 1);
    CHECK((*seen)["seats"][0]["kind"] == "open");

    // The new host may do what a host does; the old one is at no table.
    online.ask(2, online.setSeat(0, Level::Random), ofType("tableState"));
    CHECK(online.refusal(1, test::typed("startGame"))["code"] == "illegal");

    online.ask(2, test::typed("leaveTable"), ofType("sync"));
    CHECK(online.service->registry().findTable(online.code));
    online.ask(3, test::typed("leaveTable"), ofType("sync"));
    CHECK_FALSE(online.service->registry().findTable(online.code));
    CHECK(online.service->registry().tableCount() == 0);

    // Its code leads nowhere now.
    CHECK(online.refusal(1, online.join("Ana"))["code"] == "notFound");
}

TEST_CASE("Online, presence is sent as people come and go, and not for a replaced tab", "[online][presence]")
{
    Online online;
    online.open(1);
    online.open(2);
    online.create(1, tableSettings(3, 2), "Ana");
    online.ask(2, online.join("Bea"), ofType("sync"));

    std::size_t before = online.mark(1);
    online.tabs.at(2)->drop();
    auto seen = online.waitFrom(1, before, ofType("tableState"));
    REQUIRE(seen);
    CHECK((*seen)["seats"][1]["presence"] == "away");

    // Back in a new tab: their sync first, then the others are told.
    before = online.mark(1);
    test::Client& back = *(online.tabs[12] = std::make_unique<test::Client>(online.service, online.recorder, 12));
    back.hello(online.tokens.at(2));
    const auto sync = online.waitFrom(12, 0, ofType("sync"));
    REQUIRE(sync);
    CHECK((*sync)["table"]["seats"][1]["you"] == true);
    seen = online.waitFrom(1, before, ofType("tableState"));
    REQUIRE(seen);
    CHECK((*seen)["seats"][1]["presence"] == "here");

    // A second tab takes the member over; the first one's close says nothing.
    before = online.mark(1);
    test::Client& newer = *(online.tabs[22] = std::make_unique<test::Client>(online.service, online.recorder, 22));
    newer.hello(online.tokens.at(2));
    REQUIRE(online.recorder.waitForClose(12, patience));
    online.tabs.at(12)->drop();
    std::this_thread::sleep_for(quiet);
    const std::vector<Json::Value> since = online.recorder.messagesTo(1);
    for(std::size_t i = before ; i < since.size() ; i++)
        CHECK(since[i]["seats"][1]["presence"] != "away");

    // Once the table has closed, nobody hears of it again.
    online.ask(1, test::typed("leaveTable"), ofType("sync"));
    online.ask(22, test::typed("leaveTable"), ofType("sync"));
    before = online.mark(1);
    online.tabs.at(22)->drop();
    std::this_thread::sleep_for(quiet);
    CHECK(online.mark(1) == before);
}

TEST_CASE("Online, three people and three computer players play a game to the end, holding after each round",
          "[online][game]")
{
    Online online;
    for(MemberId label : {1, 2, 3})
        online.open(label);

    Settings settings = tableSettings(6, 4);
    settings.waitAfterRound = true;
    settings.botSpeed = fastestSpeed;
    online.create(1, settings, "Ana");
    online.ask(2, online.join("Bogdan", "male"), ofType("sync"));
    online.ask(3, online.join("Cora"), ofType("sync"));
    online.ask(1, online.setSeat(3, Level::ReckonerAlpha), ofType("tableState"));

    std::vector<std::unique_ptr<test::ScriptedHuman>> humans;
    for(MemberId label : {1, 2, 3})
    {
        test::Client& tab = *online.tabs.at(label);
        humans.push_back(std::make_unique<test::ScriptedHuman>(online.recorder, label, tab.answerer(),
                                                               test::Behaviour::Good, test::ScriptedHuman::OnPrompt{},
                                                               tab.continuer()));
    }
    online.send(1, test::typed("startGame"));

    for(const auto& human : humans)
    {
        REQUIRE(human->waitUntilOver(patience));
        CHECK(human->problems().empty());
    }

    const auto table = online.service->registry().findTable(online.code);
    REQUIRE(table);
    std::size_t rounds = 0;
    for(const Json::Value& message : online.recorder.messagesTo(1))
    {
        if(message["type"] == "game" && message["event"]["kind"] == "roundScored")
            rounds++;
    }
    CHECK(rounds > 0);
    for(const auto& human : humans)
        CHECK(human->counts().holds == rounds);

    for(MemberId label : {1, 2, 3})
    {
        const auto messages = online.recorder.messagesTo(label);
        checkShapes(messages);

        // The table's default turn timer reaches every person's prompt.
        for(const Json::Value& message : messages)
        {
            if(message["type"] != "prompt")
                continue;
            REQUIRE(message["deadline"].isIntegral());
            CHECK(message["deadline"].asInt64() > 0);
            CHECK(message["deadline"].asInt64() <= 60000);
        }
        const auto over = std::ranges::find_if(messages, [](const Json::Value& m)
        {
            return m["type"] == "game" && m["event"]["kind"] == "gameOver";
        });
        REQUIRE(over != messages.end());
        CHECK((*over)["snapshot"]["you"].asUInt() == label - 1);
    }
}

TEST_CASE("Online, a finished table is a lobby again, and plays again at the same code", "[online][game]")
{
    Online online;
    online.open(1);
    online.open(2);

    Settings settings = tableSettings(3, 2);
    settings.botSpeed = fastestSpeed;
    settings.foreheadAndHidden = false;
    online.create(1, settings, "Ana");
    online.ask(2, online.join("Bea"), ofType("sync"));

    std::vector<std::unique_ptr<test::ScriptedHuman>> humans;
    for(MemberId label : {1, 2})
    {
        test::Client& tab = *online.tabs.at(label);
        humans.push_back(std::make_unique<test::ScriptedHuman>(online.recorder, label, tab.answerer(),
                                                               test::Behaviour::Good));
    }
    online.send(1, test::typed("startGame"));
    for(const auto& human : humans)
        REQUIRE(human->waitUntilOver(patience));
    humans.clear();

    // Every seat is still filled, so nobody can join yet.
    online.open(3);
    CHECK(online.refusal(3, online.join("Cora"))["code"] == "tableFull");

    // Over: seats change again, and the game's sync goes only to someone in the same seat.
    online.ask(1, online.setSeat(2, std::nullopt), ofType("tableState"));
    CHECK(online.ask(1, online.takeSeat(2), ofType("tableState"))["state"] == "finished");
    online.tabs.at(1)->drop();
    online.tabs[13] = std::make_unique<test::Client>(online.service, online.recorder, 13);
    online.tabs.at(13)->hello(online.tokens.at(1));
    const auto sync = online.waitFrom(13, 0, ofType("sync"));
    REQUIRE(sync);
    CHECK((*sync)["table"]["seats"][2]["you"] == true);
    CHECK((*sync)["game"].isNull());
    CHECK((*sync)["snapshot"].isNull());

    // Bea did not move, so hers is the game's.
    online.tabs.at(2)->drop();
    online.tabs[12] = std::make_unique<test::Client>(online.service, online.recorder, 12);
    online.tabs.at(12)->hello(online.tokens.at(2));
    const auto hers = online.waitFrom(12, 0, ofType("sync"));
    REQUIRE(hers);
    CHECK((*hers)["game"] == 1);
    CHECK((*hers)["snapshot"]["you"] == 1);

    online.ask(13, online.setSeat(0, Level::Random), ofType("tableState"));
    const Json::Value again = online.ask(13, test::typed("startGame"), ofType("tableState"));
    CHECK(again["state"] == "playing");
    CHECK(online.service->registry().findTable(online.code)->game() == 2);
}

TEST_CASE("Online, nothing in the lobby may change while a game is on, and nobody can join", "[online][game]")
{
    Online online;
    online.open(1);
    online.open(2);
    online.open(3);

    // No timer, and nobody answers, so the game waits on its first person.
    Settings settings = tableSettings(3, 2);
    settings.turnTimer = std::chrono::milliseconds(0);
    online.create(1, settings, "Ana");
    online.ask(2, online.join("Bea"), ofType("sync"));
    const Json::Value waiting =
        online.ask(1, test::typed("startGame"), [](const Json::Value& m) { return m["type"] == "prompt"; });
    CHECK(waiting["deadline"].isNull());

    Json::Value rename = test::typed("updateProfile");
    rename["name"] = "Anca";
    rename["gender"] = "female";
    rename["avatar"] = "woman-long-red-hair";
    for(const Json::Value& message : {test::typed("leaveTable"), online.takeSeat(0), online.setSeat(2, Level::Random),
                                      rename, online.updateSettings(online.table(1)["settings"])})
    {
        INFO(message.toStyledString());
        CHECK(online.refusal(1, message)["code"] == "illegal");
    }
    CHECK(online.refusal(2, test::typed("leaveTable"))["code"] == "illegal");
    CHECK(online.refusal(3, online.join("Cora"))["code"] == "tableFull");
    CHECK(online.table(1)["state"] == "playing");

    // With no timer, nothing closed the prompt in all that time.
    for(const Json::Value& message : online.recorder.messagesTo(1))
        CHECK(message["type"] != "promptClosed");
}

TEST_CASE("Online, a tab that closes during a hold is no longer waited for; a replaced one still is",
          "[online][hold][presence]")
{
    Online online(patience);
    online.open(1);
    online.open(2);

    Settings settings = tableSettings(3, 2);
    settings.waitAfterRound = true;
    settings.botSpeed = fastestSpeed;
    online.create(1, settings, "Ana");
    online.ask(2, online.join("Bea"), ofType("sync"));

    std::vector<std::unique_ptr<test::ScriptedHuman>> humans;
    for(MemberId label : {1, 2})
        humans.push_back(std::make_unique<test::ScriptedHuman>(online.recorder, label, online.tabs.at(label)->answerer(),
                                                               test::Behaviour::Good));
    online.send(1, test::typed("startGame"));

    const auto first = online.waitFrom(1, 0, ofType("held"));
    REQUIRE(first);
    REQUIRE((*first)["waitingFor"].size() == 2);
    const unsigned int round = (*first)["round"].asUInt();

    // Bea's second tab takes her over, and her first one's close changes nothing.
    std::size_t before = online.mark(1);
    online.tabs[12] = std::make_unique<test::Client>(online.service, online.recorder, 12);
    online.tabs.at(12)->hello(online.tokens.at(2));
    REQUIRE(online.recorder.waitForClose(2, patience));
    online.tabs.at(2)->drop();
    std::this_thread::sleep_for(quiet);
    std::vector<Json::Value> since = online.recorder.messagesTo(1);
    for(std::size_t i = before ; i < since.size() ; i++)
        CHECK(since[i]["type"] != "held");

    // Her last tab closes: the hold is sent again, awaiting Ana alone.
    before = online.mark(1);
    online.tabs.at(12)->drop();
    const auto again = online.waitFrom(1, before, ofType("held"));
    REQUIRE(again);
    CHECK((*again)["round"].asUInt() == round);
    CHECK((*again)["waitingFor"].size() == 1);
    CHECK((*again)["waitingFor"][0].asUInt() == 0);

    before = online.mark(1);
    Json::Value next = test::typed("continue");
    next["round"] = round;
    online.send(1, next);
    CHECK(online.waitFrom(1, before, [](const Json::Value& m)
    {
        return m["type"] == "game" && m["event"]["kind"] == "roundComplete";
    }));
}

TEST_CASE("Online, the turn timer is 60 s unless set, 0 turns it off, and short ones are refused", "[online]")
{
    Online online;
    online.open(1);
    online.open(2);

    // A timer alone is enough: the replacement has its default.
    Settings settings = tableSettings(3, 2);
    settings.turnTimer = std::chrono::milliseconds(30000);
    online.create(1, settings, "Ana");
    CHECK(online.table(1)["settings"]["turnTimer"] == 30000);
    CHECK(online.table(1)["settings"]["replacement"] == "reckoner-beta");

    Json::Value changed = online.table(1)["settings"];
    changed.removeMember("turnTimer");
    changed.removeMember("replacement");
    CHECK(online.ask(1, online.updateSettings(changed), ofType("tableState"))["settings"]["turnTimer"] == 60000);
    changed["turnTimer"] = 0;
    CHECK(online.ask(1, online.updateSettings(changed), ofType("tableState"))["settings"]["turnTimer"] == 0);
    changed["turnTimer"] = 5000;
    CHECK(online.refusal(1, online.updateSettings(changed))["detail"] == "turnTimer");

    Json::Value create = test::typed("createTable");
    Settings fast = tableSettings(3, 2);
    fast.turnTimer = std::chrono::milliseconds(1);
    create["settings"] = encodeSettings(fast);
    create["profile"] = profile("Bea");
    CHECK(online.refusal(2, create)["detail"] == "turnTimer");
}
