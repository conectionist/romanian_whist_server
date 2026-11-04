#ifndef WHIST_CORE_PROTOCOL_H
#define WHIST_CORE_PROTOCOL_H

#include "core/Settings.h"

#include <romanian_whist/Card.h>
#include <romanian_whist/RoundType.h>
#include <romanian_whist/Trick.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace whist
{

inline constexpr int protocolVersion = 1;

using Clock = std::chrono::steady_clock;
using MemberId = std::uint64_t;

enum class ErrorCode
{
    BadMessage,
    Version,
    NotFound,
    TableFull,
    NameTaken,
    NotHost,
    NotYourTurn,
    StalePrompt,
    Illegal,
    RateLimited,
    Draining,
    OpenElsewhere
};

std::string_view errorName(ErrorCode code);

// ---- client to server ----

struct Hello
{
    int protocol = 0;
    std::optional<std::string> token;
    std::string lang;
};

struct CreateSolo
{
    Settings settings;
};

struct CreateTable
{
    Settings settings;
    Profile profile;
};

struct JoinTable
{
    std::string code;
    Profile profile;
};

struct UpdateProfile
{
    Profile profile;
};

struct TakeSeat
{
    unsigned int seat = 0;
};

struct SetSeat
{
    unsigned int seat = 0;

    // Open or Computer.
    SeatKind kind = SeatKind::Open;
    std::optional<Level> level;
};

struct UpdateSettings
{
    Settings settings;
};

struct StartGame
{
};

enum class PromptKind
{
    Bet,
    Play
};

// A bet or a play. The value stays 64 bits wide until it is checked, so a huge one cannot wrap to a
// legal answer.
struct Answer
{
    PromptKind kind = PromptKind::Bet;
    std::uint64_t prompt = 0;
    std::uint64_t value = 0;
};

struct Continue
{
    unsigned int round = 0;
};

struct Pause
{
};

struct Resume
{
};

struct LeaveTable
{
};

using ClientMessage = std::variant<Hello, CreateSolo, CreateTable, JoinTable, UpdateProfile, TakeSeat,
                                   SetSeat, UpdateSettings, StartGame, Answer, Continue, Pause, Resume,
                                   LeaveTable>;

// ---- server to client ----

// What a human is asked. Built on the game thread from the move context, so it never needs the
// engine. A blind prompt has no hand at all, so it cannot leak one.
struct Prompt
{
    std::uint64_t id = 0;
    PromptKind kind = PromptKind::Bet;
    bool blind = false;

    // Empty when blind; handSize is always set.
    std::vector<romanian_whist::Card> hand;
    std::size_t handSize = 0;

    // Bets.
    unsigned int max = 0;
    std::optional<unsigned int> forbidden;
    bool first = false;
    romanian_whist::RoundType roundType = romanian_whist::RoundType::Normal;

    // Plays: indices into the hand in the engine's order.
    std::vector<std::size_t> legal;

    std::optional<Clock::time_point> deadline;
};

struct RoundSeatResult
{
    std::optional<unsigned int> bet;
    unsigned int won = 0;

    // What the round was worth, and the total it has not yet been added to.
    int roundScore = 0;
    int total = 0;

    // Already counting this round.
    unsigned int wins = 0;
    unsigned int losses = 0;
};

// A scored round, as the desktop client's RoundResult has it.
struct RoundResult
{
    // 0-based, like the snapshot's round index.
    unsigned int index = 0;
    unsigned int count = 0;
    unsigned int tricks = 0;
    romanian_whist::RoundType type = romanian_whist::RoundType::Normal;
    std::optional<romanian_whist::Card> trump;

    // One-card rounds count for neither streak, so a sheet warns about the next round only when it
    // deals more than one card. False after the last round.
    bool nextCountsForStreaks = false;

    std::vector<RoundSeatResult> seats;
};

// The last finished trick, kept across rounds.
struct LastTrick
{
    unsigned int round = 0;
    unsigned int ledBy = 0;
    std::optional<unsigned int> winning;
    std::vector<romanian_whist::PlayedCard> cards;
};

struct StandingRow
{
    unsigned int seat = 0;
    std::string name;
    int score = 0;
    unsigned int place = 0;
};

enum class EventKind
{
    GameStarted,
    RoundStarted,
    BetRequested,
    BetPlaced,
    BettingComplete,
    TrickStarted,
    CardRequested,
    CardPlayed,
    TrickWon,
    RoundScored,
    RoundComplete,
    GameOver,
    GameStopped
};

std::string_view eventName(EventKind kind);

// One engine callback. Only the fields its kind carries are set.
struct GameEvent
{
    EventKind kind = EventKind::GameStarted;
    std::optional<unsigned int> seat{};
    std::optional<unsigned int> bet{};
    std::optional<romanian_whist::Card> card{};
    std::optional<unsigned int> trick{};
    std::optional<RoundResult> result{};
    std::vector<StandingRow> standings{};
    std::vector<StandingRow> winners{};
};

enum class TableState
{
    Lobby,
    Playing,
    Finished,
    Closed
};

std::string_view tableStateName(TableState state);

enum class Presence
{
    Here,
    Away,
    Replaced
};

std::string_view presenceName(Presence presence);

// One seat of a tableState message, as one viewer sees it.
struct TableSeatView
{
    SeatKind kind = SeatKind::Open;
    std::optional<Level> level;
    Profile profile;
    Presence presence = Presence::Here;
    bool you = false;
};

struct WaitingFor
{
    unsigned int seat = 0;
    std::chrono::milliseconds deadline{0};
};

struct TableView
{
    std::string code;
    Mode mode = Mode::Solo;
    TableState state = TableState::Lobby;
    std::optional<unsigned int> host;
    Settings settings;
    std::vector<TableSeatView> seats;
    std::optional<WaitingFor> waitingFor;

    // Solo only.
    bool paused = false;
};

struct HoldView
{
    // 0-based, like a round result's index.
    unsigned int round = 0;

    // The seats whose Continue is awaited. Empty in a watched solo game, which its watcher continues.
    std::vector<unsigned int> waitingFor;

    // None in solo, which waits for as long as it takes.
    std::optional<std::chrono::milliseconds> deadline;
};

} // namespace whist

#endif
