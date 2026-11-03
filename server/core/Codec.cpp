#include "core/Codec.h"

#include "core/Cards.h"

#include <json/reader.h>
#include <json/writer.h>

#include <algorithm>
#include <array>
#include <limits>
#include <memory>
#include <set>
#include <utility>

namespace whist
{

using romanian_whist::Card;
using romanian_whist::GamePhase;
using romanian_whist::GameSnapshot;
using romanian_whist::GameStatus;
using romanian_whist::GameStructure;
using romanian_whist::RoundType;
using romanian_whist::SeatSnapshot;

namespace
{

// ---- names ----

std::string_view statusName(GameStatus status)
{
    switch(status)
    {
        case GameStatus::NotStarted: return "notStarted";
        case GameStatus::InProgress: return "inProgress";
        case GameStatus::Finished: return "finished";
        case GameStatus::Stopped: return "stopped";
    }
    return "notStarted";
}

std::string_view phaseName(GamePhase phase)
{
    switch(phase)
    {
        case GamePhase::NotStarted: return "notStarted";
        case GamePhase::Betting: return "betting";
        case GamePhase::Playing: return "playing";
        case GamePhase::RoundScored: return "roundScored";
        case GamePhase::GameOver: return "gameOver";
    }
    return "notStarted";
}

std::string_view roundTypeName(RoundType type)
{
    switch(type)
    {
        case RoundType::Normal: return "normal";
        case RoundType::Forehead: return "forehead";
        case RoundType::Hidden: return "hidden";
    }
    return "normal";
}

std::string_view structureName(GameStructure structure)
{
    return structure == GameStructure::S_181 ? "1-8-1" : "8-1-8";
}

std::string_view seatKindName(SeatKind kind)
{
    switch(kind)
    {
        case SeatKind::Human: return "human";
        case SeatKind::Computer: return "computer";
        case SeatKind::Open: return "open";
    }
    return "open";
}

std::string_view modeName(Mode mode)
{
    return mode == Mode::Solo ? "solo" : "online";
}

// ---- decoding ----

// Thrown inside the decoder only, and caught at its top.
struct BadField
{
    std::string field;
};

// Reads one JSON object, and refuses any key nothing asked for.
class Fields
{
public:
    Fields(const Json::Value& object, std::string where) : object_(object), where_(std::move(where))
    {
        if(!object_.isObject())
            throw BadField{where_};
    }

    const Json::Value* optional(const char* key)
    {
        seen_.insert(key);
        const Json::Value* value = object_.find(key, key + std::char_traits<char>::length(key));
        if(value == nullptr || value->isNull())
            return nullptr;
        return value;
    }

    const Json::Value& required(const char* key)
    {
        const Json::Value* value = optional(key);
        if(value == nullptr)
            throw BadField{key};
        return *value;
    }

    void done() const
    {
        for(const std::string& key : object_.getMemberNames())
        {
            if(!seen_.contains(key))
                throw BadField{key};
        }
    }

private:
    const Json::Value& object_;
    std::string where_;
    std::set<std::string> seen_;
};

// jsoncpp's isUInt64() accepts 3.0, so the type is checked first.
std::uint64_t wholeNumber(const Json::Value& value, const char* field)
{
    if((value.type() != Json::intValue && value.type() != Json::uintValue) || !value.isUInt64())
        throw BadField{field};
    return value.asUInt64();
}

unsigned int smallNumber(const Json::Value& value, const char* field, std::uint64_t max)
{
    const std::uint64_t number = wholeNumber(value, field);
    if(number > max)
        throw BadField{field};
    return static_cast<unsigned int>(number);
}

bool boolean(const Json::Value& value, const char* field)
{
    if(!value.isBool())
        throw BadField{field};
    return value.asBool();
}

std::string text(const Json::Value& value, const char* field, std::size_t maxBytes = 256)
{
    if(!value.isString())
        throw BadField{field};

    std::string result = value.asString();
    if(result.size() > maxBytes || !isValidUtf8(result))
        throw BadField{field};
    return result;
}

Level level(const Json::Value& value, const char* field)
{
    const std::optional<Level> parsed = parseLevel(text(value, field));
    if(!parsed)
        throw BadField{field};
    return *parsed;
}

Profile profileFrom(Fields& fields)
{
    Profile profile;
    profile.name = text(fields.required("name"), "name");
    profile.gender = text(fields.required("gender"), "gender");
    profile.avatar = text(fields.required("avatar"), "avatar");
    return profile;
}

Profile profile(const Json::Value& value)
{
    Fields fields(value, "profile");
    Profile result = profileFrom(fields);
    fields.done();
    return result;
}

SeatSetting seatSetting(const Json::Value& value)
{
    Fields fields(value, "seats");
    SeatSetting seat;

    const std::string kind = text(fields.required("kind"), "kind");
    if(kind == "human")
    {
        seat.kind = SeatKind::Human;
        seat.profile = profileFrom(fields);
    }
    else if(kind == "computer")
    {
        seat.kind = SeatKind::Computer;
        seat.level = level(fields.required("level"), "level");
        seat.profile = profileFrom(fields);
    }
    else if(kind == "open")
        seat.kind = SeatKind::Open;
    else
        throw BadField{"kind"};

    fields.done();
    return seat;
}

Settings settingsFrom(const Json::Value& value)
{
    Fields fields(value, "settings");
    Settings settings;

    const Json::Value& seats = fields.required("seats");
    if(!seats.isArray() || seats.size() > 6)
        throw BadField{"seats"};
    for(const Json::Value& seat : seats)
        settings.seats.push_back(seatSetting(seat));

    const std::string structure = text(fields.required("structure"), "structure");
    if(structure == "1-8-1")
        settings.structure = GameStructure::S_181;
    else if(structure == "8-1-8")
        settings.structure = GameStructure::S_818;
    else
        throw BadField{"structure"};

    if(const Json::Value* v = fields.optional("foreheadAndHidden"))
        settings.foreheadAndHidden = boolean(*v, "foreheadAndHidden");
    if(const Json::Value* v = fields.optional("allOneCardForehead"))
        settings.allOneCardForehead = boolean(*v, "allOneCardForehead");
    if(const Json::Value* v = fields.optional("botSpeed"))
        settings.botSpeed = static_cast<int>(smallNumber(*v, "botSpeed", fastestSpeed));
    if(const Json::Value* v = fields.optional("waitAfterRound"))
        settings.waitAfterRound = boolean(*v, "waitAfterRound");
    if(const Json::Value* v = fields.optional("seed"))
        settings.seed = smallNumber(*v, "seed", std::numeric_limits<std::uint32_t>::max());
    if(const Json::Value* v = fields.optional("replacement"))
        settings.replacement = level(*v, "replacement");
    if(const Json::Value* v = fields.optional("turnTimer"))
        settings.turnTimer = std::chrono::milliseconds(smallNumber(*v, "turnTimer", 3'600'000));

    fields.done();
    return settings;
}

bool isBase64Url(std::string_view value)
{
    return std::ranges::all_of(value, [](char c)
    {
        return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' ||
               c == '_';
    });
}

ClientMessage decodeObject(const Json::Value& root)
{
    Fields fields(root, "type");
    const std::string type = text(fields.required("type"), "type", 32);

    const auto finish = [&](ClientMessage message)
    {
        fields.done();
        return message;
    };

    if(type == "hello")
    {
        Hello hello;
        hello.protocol = static_cast<int>(smallNumber(fields.required("protocol"), "protocol", 1'000'000));
        if(const Json::Value* token = fields.optional("token"))
        {
            hello.token = text(*token, "token", 22);
            if(hello.token->size() != 22 || !isBase64Url(*hello.token))
                throw BadField{"token"};
        }
        hello.lang = text(fields.required("lang"), "lang", 8);
        if(hello.lang != "en" && hello.lang != "ro")
            throw BadField{"lang"};
        return finish(hello);
    }
    if(type == "createSolo")
        return finish(CreateSolo{settingsFrom(fields.required("settings"))});
    if(type == "createTable")
        return finish(CreateTable{settingsFrom(fields.required("settings")), profile(fields.required("profile"))});
    if(type == "joinTable")
    {
        JoinTable join;
        join.code = text(fields.required("code"), "code", 6);
        if(join.code.size() != 6 || !std::ranges::all_of(join.code, [](char c)
           {
               return (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
           }))
            throw BadField{"code"};
        join.profile = profile(fields.required("profile"));
        return finish(join);
    }
    if(type == "updateProfile")
        return finish(UpdateProfile{profileFrom(fields)});
    if(type == "takeSeat")
        return finish(TakeSeat{smallNumber(fields.required("seat"), "seat", 5)});
    if(type == "setSeat")
    {
        SetSeat set;
        set.seat = smallNumber(fields.required("seat"), "seat", 5);
        const std::string kind = text(fields.required("kind"), "kind");
        if(kind == "open")
            set.kind = SeatKind::Open;
        else if(kind == "computer")
        {
            set.kind = SeatKind::Computer;
            set.level = level(fields.required("level"), "level");
        }
        else
            throw BadField{"kind"};
        return finish(set);
    }
    if(type == "updateSettings")
        return finish(UpdateSettings{settingsFrom(fields.required("settings"))});
    if(type == "startGame")
        return finish(StartGame{});
    if(type == "bet" || type == "play")
    {
        Answer answer;
        answer.kind = type == "bet" ? PromptKind::Bet : PromptKind::Play;
        answer.prompt = wholeNumber(fields.required("prompt"), "prompt");
        answer.value = type == "bet" ? wholeNumber(fields.required("value"), "value")
                                     : wholeNumber(fields.required("index"), "index");
        return finish(answer);
    }
    if(type == "continue")
        return finish(Continue{smallNumber(fields.required("round"), "round", 1000)});
    if(type == "pause")
        return finish(Pause{});
    if(type == "resume")
        return finish(Resume{});
    if(type == "leaveTable")
        return finish(LeaveTable{});

    throw BadField{"type"};
}

// ---- encoding ----

Json::Value optionalNumber(const std::optional<unsigned int>& value)
{
    return value ? Json::Value(*value) : Json::Value(Json::nullValue);
}

Json::Value optionalCard(const std::optional<Card>& card)
{
    return card ? Json::Value(cardCode(*card)) : Json::Value(Json::nullValue);
}

Json::Value optionalSeat(const std::optional<romanian_whist::Seat>& seat)
{
    return seat ? Json::Value(seat->index) : Json::Value(Json::nullValue);
}

Json::Value cards(const std::vector<Card>& hand)
{
    Json::Value list(Json::arrayValue);
    for(const Card& card : hand)
        list.append(cardCode(card));
    return list;
}

Json::Value playedCards(const std::vector<romanian_whist::PlayedCard>& played,
                        const std::optional<unsigned int>& winning)
{
    Json::Value list(Json::arrayValue);
    for(const romanian_whist::PlayedCard& entry : played)
    {
        Json::Value item(Json::objectValue);
        item["seat"] = entry.seat.index;
        item["card"] = cardCode(entry.card);
        item["winning"] = winning && *winning == entry.seat.index;
        list.append(item);
    }
    return list;
}

Json::Value profileJson(const Profile& profile)
{
    Json::Value value(Json::objectValue);
    value["name"] = profile.name;
    value["gender"] = profile.gender;
    value["avatar"] = profile.avatar;
    return value;
}

Json::Value standings(const std::vector<StandingRow>& rows)
{
    Json::Value list(Json::arrayValue);
    for(const StandingRow& row : rows)
    {
        Json::Value item(Json::objectValue);
        item["seat"] = row.seat;
        item["name"] = row.name;
        item["score"] = row.score;
        item["place"] = row.place;
        list.append(item);
    }
    return list;
}

Json::Value milliseconds(std::chrono::milliseconds value)
{
    return static_cast<Json::Int64>(value.count());
}

Json::Value typed(std::string_view type)
{
    Json::Value message(Json::objectValue);
    message["type"] = std::string(type);
    return message;
}

} // namespace

std::string_view errorName(ErrorCode code)
{
    switch(code)
    {
        case ErrorCode::BadMessage: return "badMessage";
        case ErrorCode::Version: return "version";
        case ErrorCode::NotFound: return "notFound";
        case ErrorCode::TableFull: return "tableFull";
        case ErrorCode::NameTaken: return "nameTaken";
        case ErrorCode::NotHost: return "notHost";
        case ErrorCode::NotYourTurn: return "notYourTurn";
        case ErrorCode::StalePrompt: return "stalePrompt";
        case ErrorCode::Illegal: return "illegal";
        case ErrorCode::RateLimited: return "rateLimited";
        case ErrorCode::Draining: return "draining";
    }
    return "badMessage";
}

std::string_view eventName(EventKind kind)
{
    switch(kind)
    {
        case EventKind::GameStarted: return "gameStarted";
        case EventKind::RoundStarted: return "roundStarted";
        case EventKind::BetRequested: return "betRequested";
        case EventKind::BetPlaced: return "betPlaced";
        case EventKind::BettingComplete: return "bettingComplete";
        case EventKind::TrickStarted: return "trickStarted";
        case EventKind::CardRequested: return "cardRequested";
        case EventKind::CardPlayed: return "cardPlayed";
        case EventKind::TrickWon: return "trickWon";
        case EventKind::RoundScored: return "roundScored";
        case EventKind::RoundComplete: return "roundComplete";
        case EventKind::GameOver: return "gameOver";
        case EventKind::GameStopped: return "gameStopped";
    }
    return "gameStarted";
}

std::string_view tableStateName(TableState state)
{
    switch(state)
    {
        case TableState::Lobby: return "lobby";
        case TableState::Playing: return "playing";
        case TableState::Finished: return "finished";
        case TableState::Closed: return "closed";
    }
    return "closed";
}

std::string_view presenceName(Presence presence)
{
    switch(presence)
    {
        case Presence::Here: return "here";
        case Presence::Away: return "away";
        case Presence::Replaced: return "replaced";
    }
    return "here";
}

Decoded decodeClientMessage(std::string_view message)
{
    Decoded decoded;

    if(!isValidUtf8(message))
        return decoded;

    Json::CharReaderBuilder builder;
    Json::CharReaderBuilder::strictMode(&builder.settings_);
    builder["stackLimit"] = 16;
    const std::unique_ptr<Json::CharReader> reader(builder.newCharReader());

    Json::Value root;
    try
    {
        std::string errors;
        if(!reader->parse(message.data(), message.data() + message.size(), &root, &errors))
            return decoded;

        decoded.message = decodeObject(root);
    }
    catch(const BadField& bad)
    {
        decoded.detail = bad.field;
    }
    catch(const Json::Exception&)
    {
    }
    return decoded;
}

Settings decodeSettings(const Json::Value& value)
{
    return settingsFrom(value);
}

std::string toText(const Json::Value& message)
{
    Json::StreamWriterBuilder builder;
    builder["indentation"] = "";
    builder["emitUTF8"] = true;
    return Json::writeString(builder, message);
}

Json::Value encodeSnapshot(const GameSnapshot& snapshot)
{
    Json::Value value(Json::objectValue);
    value["you"] = optionalSeat(snapshot.viewer);
    value["status"] = std::string(statusName(snapshot.status));
    value["phase"] = std::string(phaseName(snapshot.phase));

    Json::Value round(Json::objectValue);
    round["index"] = snapshot.roundIndex;
    round["count"] = snapshot.roundCount;
    round["type"] = std::string(roundTypeName(snapshot.roundType));
    round["tricks"] = snapshot.trickCount;
    round["trick"] = snapshot.trickNumber;
    round["trump"] = optionalCard(snapshot.trump);
    round["leader"] = optionalSeat(snapshot.roundLeader);
    value["round"] = round;

    value["active"] = optionalSeat(snapshot.activeSeat);
    value["forbiddenBet"] = optionalNumber(snapshot.forbiddenBet);

    Json::Value bets(Json::objectValue);
    bets["placed"] = snapshot.betsPlaced;
    bets["sum"] = snapshot.betSum;
    value["bets"] = bets;

    Json::Value table(Json::objectValue);
    table["ledBy"] = optionalSeat(snapshot.ledBy);
    table["leadSuit"] = snapshot.leadSuit ? Json::Value(suitCode(*snapshot.leadSuit)) : Json::Value(Json::nullValue);
    std::optional<unsigned int> winning;
    if(snapshot.winning)
        winning = snapshot.winning->index;
    table["cards"] = playedCards(snapshot.table, winning);
    value["table"] = table;

    Json::Value seats(Json::arrayValue);
    for(const SeatSnapshot& seat : snapshot.seats)
    {
        Json::Value item(Json::objectValue);
        item["name"] = seat.name;
        item["bidOrder"] = seat.biddingOrder;
        item["bet"] = optionalNumber(seat.bet);
        item["won"] = seat.tricksWon;
        item["cards"] = seat.cardsLeft;
        item["roundScore"] = seat.roundScore;
        item["total"] = seat.totalScore;
        item["wins"] = seat.winStreak;
        item["losses"] = seat.lossStreak;

        // Absent, never present and empty, when the viewer may not see it.
        if(seat.hand)
            item["hand"] = cards(*seat.hand);
        seats.append(item);
    }
    value["seats"] = seats;
    return value;
}

Json::Value encodeRoundResult(const RoundResult& result)
{
    Json::Value value(Json::objectValue);
    value["index"] = result.index;
    value["count"] = result.count;
    value["tricks"] = result.tricks;
    value["type"] = std::string(roundTypeName(result.type));
    value["trump"] = optionalCard(result.trump);
    value["nextCountsForStreaks"] = result.nextCountsForStreaks;

    Json::Value seats(Json::arrayValue);
    for(const RoundSeatResult& seat : result.seats)
    {
        Json::Value item(Json::objectValue);
        item["bet"] = optionalNumber(seat.bet);
        item["won"] = seat.won;
        item["roundScore"] = seat.roundScore;
        item["total"] = seat.total;
        item["wins"] = seat.wins;
        item["losses"] = seat.losses;
        seats.append(item);
    }
    value["seats"] = seats;
    return value;
}

Json::Value encodeLastTrick(const LastTrick& trick)
{
    Json::Value value(Json::objectValue);
    value["round"] = trick.round;
    value["ledBy"] = trick.ledBy;
    value["cards"] = playedCards(trick.cards, trick.winning);
    return value;
}

Json::Value encodeEvent(const GameEvent& event)
{
    Json::Value value(Json::objectValue);
    value["kind"] = std::string(eventName(event.kind));

    switch(event.kind)
    {
        case EventKind::BetRequested:
        case EventKind::CardRequested:
            value["seat"] = event.seat.value_or(0);
            break;
        case EventKind::BetPlaced:
            value["seat"] = event.seat.value_or(0);
            value["bet"] = event.bet.value_or(0);
            break;
        case EventKind::TrickStarted:
            value["trick"] = event.trick.value_or(0);
            value["leader"] = event.seat.value_or(0);
            break;
        case EventKind::CardPlayed:
            value["seat"] = event.seat.value_or(0);
            value["card"] = optionalCard(event.card);
            break;
        case EventKind::TrickWon:
            value["seat"] = event.seat.value_or(0);
            value["trick"] = event.trick.value_or(0);
            break;
        case EventKind::RoundScored:
            value["result"] = event.result ? encodeRoundResult(*event.result) : Json::Value(Json::nullValue);
            break;
        case EventKind::GameOver:
            value["standings"] = standings(event.standings);
            value["winners"] = standings(event.winners);
            break;
        default:
            break;
    }
    return value;
}

Json::Value encodeSettings(const Settings& settings)
{
    Json::Value value(Json::objectValue);

    Json::Value seats(Json::arrayValue);
    for(const SeatSetting& seat : settings.seats)
    {
        Json::Value item(Json::objectValue);
        item["kind"] = std::string(seatKindName(seat.kind));
        if(seat.kind != SeatKind::Open)
        {
            item["name"] = seat.profile.name;
            item["gender"] = seat.profile.gender;
            item["avatar"] = seat.profile.avatar;
        }
        if(seat.level)
            item["level"] = std::string(levelName(*seat.level));
        seats.append(item);
    }
    value["seats"] = seats;

    value["structure"] = std::string(structureName(settings.structure));
    value["foreheadAndHidden"] = settings.foreheadAndHidden;
    value["allOneCardForehead"] = settings.allOneCardForehead;
    value["botSpeed"] = settings.botSpeed;
    value["waitAfterRound"] = settings.waitAfterRound;
    if(settings.seed)
        value["seed"] = *settings.seed;
    if(settings.replacement)
        value["replacement"] = std::string(levelName(*settings.replacement));
    if(settings.turnTimer)
        value["turnTimer"] = milliseconds(*settings.turnTimer);
    return value;
}

Json::Value encodeTableView(const TableView& view)
{
    Json::Value value(Json::objectValue);
    value["code"] = view.code;
    value["mode"] = std::string(modeName(view.mode));
    value["state"] = std::string(tableStateName(view.state));
    value["host"] = optionalNumber(view.host);
    value["settings"] = encodeSettings(view.settings);

    Json::Value seats(Json::arrayValue);
    for(const TableSeatView& seat : view.seats)
    {
        Json::Value item(Json::objectValue);
        item["kind"] = std::string(seatKindName(seat.kind));
        if(seat.kind == SeatKind::Human)
        {
            item = profileJson(seat.profile);
            item["kind"] = "human";
            item["presence"] = std::string(presenceName(seat.presence));
            item["you"] = seat.you;
        }
        else if(seat.kind == SeatKind::Computer)
        {
            item = profileJson(seat.profile);
            item["kind"] = "computer";
            item["level"] = std::string(levelName(seat.level.value_or(Level::LowRisk)));
        }
        seats.append(item);
    }
    value["seats"] = seats;

    if(view.waitingFor)
    {
        Json::Value waiting(Json::objectValue);
        waiting["seat"] = view.waitingFor->seat;
        waiting["deadline"] = milliseconds(view.waitingFor->deadline);
        value["waitingFor"] = waiting;
    }
    return value;
}

Json::Value encodeHold(const HoldView& hold)
{
    Json::Value value(Json::objectValue);
    value["round"] = hold.round;
    Json::Value waiting(Json::arrayValue);
    for(unsigned int seat : hold.waitingFor)
        waiting.append(seat);
    value["waitingFor"] = waiting;
    value["deadline"] = milliseconds(hold.deadline);
    return value;
}

Json::Value encodePrompt(const Prompt& prompt, Clock::time_point now)
{
    Json::Value value(Json::objectValue);
    value["id"] = static_cast<Json::UInt64>(prompt.id);
    value["kind"] = prompt.kind == PromptKind::Bet ? "bet" : "play";

    if(prompt.deadline)
    {
        // Rounded up, so a prompt sent at once reads as its whole timer.
        const auto left = std::chrono::ceil<std::chrono::milliseconds>(*prompt.deadline - now);
        value["deadline"] = milliseconds(std::max(left, std::chrono::milliseconds(0)));
    }
    else
        value["deadline"] = Json::nullValue;

    if(prompt.blind)
        value["handSize"] = static_cast<Json::UInt64>(prompt.handSize);
    else
        value["hand"] = cards(prompt.hand);

    if(prompt.kind == PromptKind::Bet)
    {
        value["max"] = prompt.max;
        value["forbidden"] = optionalNumber(prompt.forbidden);
        value["first"] = prompt.first;
        value["roundType"] = std::string(roundTypeName(prompt.roundType));
    }
    else
    {
        Json::Value legal(Json::arrayValue);
        for(std::size_t index : prompt.legal)
            legal.append(static_cast<Json::UInt64>(index));
        value["legal"] = legal;
        value["blind"] = prompt.blind;
    }
    return value;
}

Json::Value welcomeMessage(std::string_view token, MemberId member, std::string_view version,
                           std::string_view commit)
{
    Json::Value message = typed("welcome");
    message["token"] = std::string(token);
    message["member"] = static_cast<Json::UInt64>(member);
    Json::Value server(Json::objectValue);
    server["version"] = std::string(version);
    server["commit"] = std::string(commit);
    message["server"] = server;
    return message;
}

Json::Value tableStateMessage(const TableView& view)
{
    Json::Value message = encodeTableView(view);
    message["type"] = "tableState";
    return message;
}

Json::Value gameMessage(unsigned int game, const GameEvent& event, Json::Value snapshot)
{
    Json::Value message = typed("game");
    message["game"] = game;
    message["event"] = encodeEvent(event);
    message["snapshot"] = std::move(snapshot);
    return message;
}

Json::Value promptMessage(const Prompt& prompt, Clock::time_point now)
{
    Json::Value message = encodePrompt(prompt, now);
    message["type"] = "prompt";
    return message;
}

Json::Value promptClosedMessage(std::uint64_t prompt, bool byYou)
{
    Json::Value message = typed("promptClosed");
    message["id"] = static_cast<Json::UInt64>(prompt);
    message["by"] = byYou ? "you" : "computer";
    return message;
}

Json::Value heldMessage(const HoldView& hold)
{
    Json::Value message = encodeHold(hold);
    message["type"] = "held";
    return message;
}

Json::Value errorMessage(ErrorCode code, std::string_view detail)
{
    Json::Value message = typed("error");
    message["code"] = std::string(errorName(code));
    if(!detail.empty())
        message["detail"] = std::string(detail);
    return message;
}

Json::Value syncMessage(const SyncView& sync, Clock::time_point now)
{
    Json::Value message = typed("sync");
    message["table"] = encodeTableView(sync.table);
    message["game"] = optionalNumber(sync.game);
    message["snapshot"] = sync.snapshot ? *sync.snapshot : Json::Value(Json::nullValue);

    Json::Value history(Json::arrayValue);
    for(const RoundResult& result : sync.history)
        history.append(encodeRoundResult(result));
    message["history"] = history;

    message["lastTrick"] = sync.lastTrick ? encodeLastTrick(*sync.lastTrick) : Json::Value(Json::nullValue);
    message["prompt"] = sync.prompt ? encodePrompt(*sync.prompt, now) : Json::Value(Json::nullValue);
    message["held"] = sync.held ? encodeHold(*sync.held) : Json::Value(Json::nullValue);
    return message;
}

} // namespace whist
