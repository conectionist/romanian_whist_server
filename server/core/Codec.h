#ifndef WHIST_CORE_CODEC_H
#define WHIST_CORE_CODEC_H

#include "core/Protocol.h"

#include <romanian_whist/Snapshot.h>

#include <json/value.h>

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace whist
{

// ---- decoding what clients send ----

struct Decoded
{
    std::optional<ClientMessage> message;

    // Set when message is empty. detail names the field at fault and never carries a value.
    ErrorCode error = ErrorCode::BadMessage;
    std::string detail;
};

// Strict: one JSON object, a known type, known fields only, whole numbers where numbers are
// asked for, and valid UTF-8 throughout.
Decoded decodeClientMessage(std::string_view text);

Settings decodeSettings(const Json::Value& value);

// ---- encoding what the server sends ----

// One line, UTF-8 as it is.
std::string toText(const Json::Value& message);

Json::Value encodeSnapshot(const romanian_whist::GameSnapshot& snapshot);
Json::Value encodeEvent(const GameEvent& event);
Json::Value encodeSettings(const Settings& settings);
Json::Value encodeRoundResult(const RoundResult& result);
Json::Value encodeLastTrick(const LastTrick& trick);
Json::Value encodeTableView(const TableView& view);
// A hold's deadline goes out as the milliseconds left at `now`, or null without one.
Json::Value encodeHold(const HoldView& hold, Clock::time_point now);

// A prompt's deadline goes out as the milliseconds left at `now`, or null without one.
Json::Value encodePrompt(const Prompt& prompt, Clock::time_point now);

Json::Value welcomeMessage(std::string_view token, MemberId member, std::string_view version,
                           std::string_view commit);
Json::Value tableStateMessage(const TableView& view);
Json::Value gameMessage(unsigned int game, const GameEvent& event, Json::Value snapshot);
Json::Value promptMessage(const Prompt& prompt, Clock::time_point now);
Json::Value promptClosedMessage(std::uint64_t prompt, bool byYou);
Json::Value heldMessage(const HoldView& hold, Clock::time_point now);
Json::Value errorMessage(ErrorCode code, std::string_view detail = {});

// What a viewer needs to pick a game up where it is, without the engine.
struct SyncView
{
    // None for a member at no table.
    std::optional<TableView> table;
    std::optional<unsigned int> game;
    std::optional<Json::Value> snapshot;
    std::vector<RoundResult> history;
    std::optional<LastTrick> lastTrick;
    std::optional<Prompt> prompt;
    std::optional<HoldView> held;
};

Json::Value syncMessage(const SyncView& sync, Clock::time_point now);

} // namespace whist

#endif
