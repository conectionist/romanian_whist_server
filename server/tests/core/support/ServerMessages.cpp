#include "support/ServerMessages.h"

#include "core/Cards.h"

#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace whist::test
{

namespace
{

using Check = std::function<std::string(const Json::Value&)>;

bool isWhole(const Json::Value& v)
{
    return v.type() == Json::intValue || v.type() == Json::uintValue;
}

Check whole()
{
    return [](const Json::Value& v) { return isWhole(v) && v.isUInt64() ? "" : "not a whole number"; };
}

Check integer()
{
    return [](const Json::Value& v) { return isWhole(v) ? "" : "not an integer"; };
}

Check boolean()
{
    return [](const Json::Value& v) { return v.isBool() ? "" : "not a boolean"; };
}

Check text()
{
    return [](const Json::Value& v) { return v.isString() ? "" : "not a string"; };
}

Check oneOf(std::set<std::string> values)
{
    return [values](const Json::Value& v)
    {
        return v.isString() && values.contains(v.asString()) ? "" : "not one of the allowed names";
    };
}

Check card()
{
    return [](const Json::Value& v) { return v.isString() && parseCard(v.asString()) ? "" : "not a card"; };
}

Check suit()
{
    return oneOf({"H", "D", "S", "C"});
}

Check nullable(Check inner)
{
    return [inner](const Json::Value& v) { return v.isNull() ? std::string() : inner(v); };
}

Check list(Check item)
{
    return [item](const Json::Value& v) -> std::string
    {
        if(!v.isArray())
            return "not an array";
        for(const Json::Value& entry : v)
        {
            if(std::string error = item(entry); !error.empty())
                return "[]: " + error;
        }
        return "";
    };
}

Check object(std::map<std::string, Check> required, std::map<std::string, Check> optional = {})
{
    return [required, optional](const Json::Value& v) -> std::string
    {
        if(!v.isObject())
            return "not an object";
        for(const auto& [key, check] : required)
        {
            if(!v.isMember(key))
                return key + ": missing";
            if(std::string error = check(v[key]); !error.empty())
                return key + ": " + error;
        }
        for(const std::string& key : v.getMemberNames())
        {
            if(required.contains(key))
                continue;
            const auto found = optional.find(key);
            if(found == optional.end())
                return key + ": not expected";
            if(std::string error = found->second(v[key]); !error.empty())
                return key + ": " + error;
        }
        return "";
    };
}

Check level()
{
    return oneOf({"random", "first-card", "low-risk", "ducking", "cyborg", "reckoner-alpha", "reckoner-beta",
                  "reckoner-gamma", "reckoner-delta"});
}

Check roundType()
{
    return oneOf({"normal", "forehead", "hidden"});
}

Check playedCards()
{
    return list(object({{"seat", whole()}, {"card", card()}, {"winning", boolean()}}));
}

Check snapshot()
{
    return object({
        {"you", nullable(whole())},
        {"status", oneOf({"notStarted", "inProgress", "finished", "stopped"})},
        {"phase", oneOf({"notStarted", "betting", "playing", "roundScored", "gameOver"})},
        {"round", object({{"index", whole()}, {"count", whole()}, {"type", roundType()}, {"tricks", whole()},
                          {"trick", whole()}, {"trump", nullable(card())}, {"leader", nullable(whole())}})},
        {"active", nullable(whole())},
        {"forbiddenBet", nullable(whole())},
        {"bets", object({{"placed", whole()}, {"sum", whole()}})},
        {"table", object({{"ledBy", nullable(whole())}, {"leadSuit", nullable(suit())}, {"cards", playedCards()}})},
        {"seats", list(object({{"name", text()}, {"bidOrder", whole()}, {"bet", nullable(whole())}, {"won", whole()},
                               {"cards", whole()}, {"roundScore", integer()}, {"total", integer()},
                               {"wins", whole()}, {"losses", whole()}},
                              {{"hand", list(card())}}))},
    });
}

Check roundResult()
{
    return object({{"index", whole()}, {"count", whole()}, {"tricks", whole()}, {"type", roundType()},
                   {"trump", nullable(card())}, {"nextCountsForStreaks", boolean()},
                   {"seats", list(object({{"bet", nullable(whole())}, {"won", whole()}, {"roundScore", integer()},
                                          {"total", integer()}, {"wins", whole()}, {"losses", whole()}}))}});
}

Check standings()
{
    return list(object({{"seat", whole()}, {"name", text()}, {"score", integer()}, {"place", whole()}}));
}

Check event()
{
    return [](const Json::Value& v) -> std::string
    {
        if(!v.isObject() || !v["kind"].isString())
            return "no kind";
        const std::string kind = v["kind"].asString();
        const Check kindCheck = oneOf({kind});
        std::map<std::string, Check> fields{{"kind", kindCheck}};

        if(kind == "betRequested" || kind == "cardRequested")
            fields["seat"] = whole();
        else if(kind == "betPlaced")
        {
            fields["seat"] = whole();
            fields["bet"] = whole();
        }
        else if(kind == "trickStarted")
        {
            fields["trick"] = whole();
            fields["leader"] = whole();
        }
        else if(kind == "cardPlayed")
        {
            fields["seat"] = whole();
            fields["card"] = card();
        }
        else if(kind == "trickWon")
        {
            fields["seat"] = whole();
            fields["trick"] = whole();
        }
        else if(kind == "roundScored")
            fields["result"] = roundResult();
        else if(kind == "gameOver")
        {
            fields["standings"] = standings();
            fields["winners"] = standings();
        }
        else if(kind != "gameStarted" && kind != "roundStarted" && kind != "bettingComplete" &&
                kind != "roundComplete" && kind != "gameStopped")
            return "unknown kind " + kind;

        return object(fields)(v);
    };
}

Check settings()
{
    const Check seat = [](const Json::Value& v) -> std::string
    {
        if(!v.isObject())
            return "not an object";
        const std::string kind = v["kind"].asString();
        if(kind == "open")
            return object({{"kind", oneOf({"open"})}})(v);
        if(kind == "human")
            return object({{"kind", text()}, {"name", text()}, {"gender", oneOf({"female", "male"})},
                           {"avatar", text()}})(v);
        if(kind == "computer")
            return object({{"kind", text()}, {"level", level()}, {"name", text()}, {"gender", oneOf({"female", "male"})},
                           {"avatar", text()}})(v);
        return "unknown seat kind";
    };

    return object({{"seats", list(seat)}, {"structure", oneOf({"1-8-1", "8-1-8"})}, {"foreheadAndHidden", boolean()},
                   {"allOneCardForehead", boolean()}, {"botSpeed", whole()}, {"waitAfterRound", boolean()}},
                  {{"seed", whole()}, {"replacement", level()}, {"turnTimer", whole()}});
}

std::map<std::string, Check> tableFields()
{
    const Check seat = [](const Json::Value& v) -> std::string
    {
        if(!v.isObject())
            return "not an object";
        const std::string kind = v["kind"].asString();
        if(kind == "open")
            return object({{"kind", text()}})(v);
        if(kind == "human")
            return object({{"kind", text()}, {"name", text()}, {"gender", oneOf({"female", "male"})}, {"avatar", text()},
                           {"presence", oneOf({"here", "away", "replaced"})}, {"you", boolean()}})(v);
        if(kind == "computer")
            return object({{"kind", text()}, {"level", level()}, {"name", text()}, {"gender", oneOf({"female", "male"})},
                           {"avatar", text()}})(v);
        return "unknown seat kind";
    };

    return {{"code", text()},
            {"mode", oneOf({"solo", "online"})},
            {"state", oneOf({"lobby", "playing", "finished", "closed"})},
            {"host", nullable(whole())},
            {"settings", settings()},
            {"seats", list(seat)},
            {"paused", boolean()}};
}

std::map<std::string, Check> tableOptional()
{
    return {{"waitingFor", object({{"seat", whole()}, {"deadline", whole()}})}};
}

Check prompt(bool withType)
{
    return [withType](const Json::Value& v) -> std::string
    {
        if(!v.isObject())
            return "not an object";
        std::map<std::string, Check> fields{{"id", whole()}, {"kind", oneOf({"bet", "play"})},
                                            {"deadline", nullable(whole())}};
        if(withType)
        {
            fields["type"] = text();
            fields["seq"] = whole();
        }
        if(v.isMember("hand"))
            fields["hand"] = list(card());
        else
            fields["handSize"] = whole();

        if(v["kind"].asString() == "bet")
        {
            fields["max"] = whole();
            fields["forbidden"] = nullable(whole());
            fields["first"] = boolean();
            fields["roundType"] = roundType();
        }
        else
        {
            fields["legal"] = list(whole());
            fields["blind"] = boolean();
        }
        return object(fields)(v);
    };
}

Check hold()
{
    return object({{"round", whole()}, {"waitingFor", list(whole())}, {"deadline", nullable(whole())}});
}

} // namespace

std::string shapeError(const Json::Value& message)
{
    if(!message.isObject() || !message["type"].isString())
        return "no type";

    const std::string type = message["type"].asString();
    const std::map<std::string, Check> envelope{{"type", text()}, {"seq", whole()}};
    const auto with = [&](std::map<std::string, Check> fields)
    {
        fields.insert(envelope.begin(), envelope.end());
        return fields;
    };

    if(type == "welcome")
        return object(with({{"token", text()}, {"member", whole()},
                            {"server", object({{"version", text()}, {"commit", text()}})}}))(message);
    if(type == "tableState")
        return object(with(tableFields()), tableOptional())(message);
    if(type == "game")
        return object(with({{"game", whole()}, {"event", event()}, {"snapshot", nullable(snapshot())}}))(message);
    if(type == "prompt")
        return prompt(true)(message);
    if(type == "promptClosed")
        return object(with({{"id", whole()}, {"by", oneOf({"you", "computer"})}}))(message);
    if(type == "held")
        return object(with({{"round", whole()}, {"waitingFor", list(whole())}, {"deadline", nullable(whole())}}))(message);
    if(type == "sync")
        return object(with({{"table", nullable(object(tableFields(), tableOptional()))},
                            {"game", nullable(whole())},
                            {"snapshot", nullable(snapshot())},
                            {"history", list(roundResult())},
                            {"lastTrick", nullable(object({{"round", whole()}, {"ledBy", whole()}, {"cards", playedCards()}}))},
                            {"prompt", nullable(prompt(false))},
                            {"held", nullable(hold())}}))(message);
    if(type == "error")
        return object(with({{"code", oneOf({"badMessage", "version", "notFound", "tableFull", "nameTaken", "notHost",
                                            "notYourTurn", "stalePrompt", "illegal", "rateLimited", "draining",
                                            "openElsewhere"})}}),
                      {{"detail", text()}})(message);

    return "unknown type " + type;
}

} // namespace whist::test
