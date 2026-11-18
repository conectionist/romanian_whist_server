#ifndef WHIST_TESTS_CORE_ONLINE_H
#define WHIST_TESTS_CORE_ONLINE_H

#include "core/Codec.h"
#include "core/Service.h"

#include "support/Client.h"
#include "support/Games.h"
#include "support/Recorder.h"
#include "support/ScriptedHuman.h"
#include "support/ServerMessages.h"

#include <catch2/catch_test_macros.hpp>

#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace whist::test
{

using std::chrono::milliseconds;


const milliseconds patience(60000 * test::slowdown);

// Long enough for a message that is coming to have come.
const milliseconds quiet(300 * test::slowdown);

using Match = std::function<bool(const Json::Value&)>;

inline Match ofType(std::string type)
{
    return [type](const Json::Value& m) { return m["type"] == type; };
}

inline Json::Value profile(const std::string& name, const std::string& gender = "female")
{
    Json::Value value(Json::objectValue);
    value["name"] = name;
    value["gender"] = gender;
    value["avatar"] = gender == "female" ? "woman-long-red-hair" : "man-dark-hair-beard";
    return value;
}

// `seats` seats: the first `open` open, the rest computer players of `level`.
inline Settings onlineSettings(unsigned int seats, unsigned int open, Level level = Level::FirstCard)
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

    // Tabs that have been replaced, kept alive for the player threads that may still be using them.
    std::mutex tabsMutex;
    std::vector<std::unique_ptr<test::Client>> retired;

    // `tweak` changes the service's options before it is made.
    explicit Online(milliseconds roundHold = milliseconds(30000),
                    const std::function<void(ServiceOptions&)>& tweak = {})
    {
        ServiceOptions options;
        options.registry.table.instantFloor = milliseconds(0);
        options.registry.table.threadNiceness = 0;
        options.registry.table.roundHold = roundHold;
        if(tweak)
            tweak(options);
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
        tab(label).send(message);
    }

    test::Client& tab(MemberId label)
    {
        std::lock_guard lock(tabsMutex);
        return *tabs.at(label);
    }

    // The member's connection closes, as when a tab is closed.
    void drop(MemberId label)
    {
        tab(label).drop();
    }

    // A new tab for a member whose last one has gone, under the same label, so a player thread
    // reading that label carries on across it. Returns once the welcome and the sync are in.
    test::Client& reopen(MemberId label)
    {
        test::Client* fresh = nullptr;
        {
            std::lock_guard lock(tabsMutex);
            retired.push_back(std::move(tabs.at(label)));
            tabs[label] = std::make_unique<test::Client>(service, recorder, label);
            fresh = tabs[label].get();
        }
        fresh->hello(tokens.at(label));
        return *fresh;
    }

    // As reopen(), but nothing waits for the sync, so a thread other than the test's may call it.
    void reconnect(MemberId label)
    {
        test::Client* fresh = nullptr;
        {
            std::lock_guard lock(tabsMutex);
            retired.push_back(std::move(tabs.at(label)));
            tabs[label] = std::make_unique<test::Client>(service, recorder, label);
            fresh = tabs[label].get();
        }
        fresh->sendHello(tokens.at(label));
    }

    // Answers and Continues through whichever tab the member has now.
    ScriptedHuman::Answerer answerer(MemberId label)
    {
        return [this, label](const Answer& answer) { tab(label).answerer()(answer); };
    }

    ScriptedHuman::Continuer continuer(MemberId label)
    {
        return [this, label](unsigned int round) { tab(label).continuer()(round); };
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

inline void checkShapes(const std::vector<Json::Value>& messages)
{
    for(const Json::Value& message : messages)
    {
        INFO(message.toStyledString());
        CHECK(test::shapeError(message).empty());
    }
}


} // namespace whist::test

#endif
