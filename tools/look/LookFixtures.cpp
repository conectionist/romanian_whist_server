#include "look/LookFixtures.h"

#include "look/DesktopRules.h"

#include "core/Cards.h"
#include "core/Catalogue.h"
#include "core/Codec.h"
#include "core/Connection.h"
#include "core/Service.h"
#include "core/Session.h"

#include <romanian_whist/Snapshot.h>

#include <json/reader.h>
#include <json/writer.h>

#include <array>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <exception>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <stdexcept>
#include <utility>

namespace whist::look
{

using romanian_whist::Card;
using romanian_whist::Rank;
using romanian_whist::Seat;
using romanian_whist::Suit;

namespace
{

// ---- the desktop's games ----

// What a picture is expected to show, as the desktop's tool states it for each shot.
struct Moment
{
    unsigned int round;
    unsigned int trick;
    std::size_t onTable;
    std::size_t inHand;
};

struct Capture
{
    // The picture's name without its extension.
    const char* name;

    // How many decisions the person makes before the picture is taken, bids and cards alike. 0 is
    // the first bid of round 1.
    int decisions;

    Moment expected;
};

// One game for each seat count, with every picture taken from it, in decision order.
struct Game
{
    unsigned int players;
    std::vector<Capture> captures;
};

const std::vector<Game>& games()
{
    static const std::vector<Game> shots = {
        {3, {{"table-3-players", 26, {8, 2, 2, 5}}}},
        {4, {{"table-4-players", 22, {8, 2, 3, 4}}}},
        {5, {{"table-5-players", 24, {9, 2, 4, 4}}}},
        {6, {{"bidding", 0, {1, 0, 0, 1}}, {"table-6-players", 26, {10, 2, 5, 4}}}},
    };
    return shots;
}

constexpr unsigned int dealSeed = 42;

struct Opponent
{
    const char* name;
    const char* gender;
};

constexpr std::array<Opponent, 5> opponents{{
    {"Ana", "female"}, {"Bogdan", "male"}, {"Cristina", "female"}, {"Dan", "male"}, {"Elena", "female"}}};

// ---- reading a game's messages ----

// The connection a game's messages go to. Posting only queues, as a real connection does, and the
// tool reads them from its own thread.
class Inbox : public Connection
{
public:
    void post(std::string text) override
    {
        {
            std::lock_guard lock(mutex_);
            messages_.push_back(std::move(text));
        }
        changed_.notify_all();
    }

    void close() override
    {
        shut();
    }

    void abort() override
    {
        shut();
    }

    // Nothing when none came in time, or when the connection closed with none left.
    std::optional<std::string> next(std::chrono::milliseconds patience)
    {
        std::unique_lock lock(mutex_);
        changed_.wait_for(lock, patience, [this] { return !messages_.empty() || closed_; });
        if(messages_.empty())
            return std::nullopt;
        std::string text = std::move(messages_.front());
        messages_.pop_front();
        return text;
    }

    bool closed() const
    {
        std::lock_guard lock(mutex_);
        return closed_;
    }

private:
    void shut()
    {
        {
            std::lock_guard lock(mutex_);
            closed_ = true;
        }
        changed_.notify_all();
    }

    mutable std::mutex mutex_;
    std::condition_variable changed_;
    std::deque<std::string> messages_;
    bool closed_ = false;
};

std::optional<Json::Value> parseMessage(const std::string& text)
{
    Json::CharReaderBuilder builder;
    Json::CharReaderBuilder::strictMode(&builder.settings_);
    const std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
    Json::Value value;
    std::string errors;
    if(!reader->parse(text.data(), text.data() + text.size(), &value, &errors))
        return std::nullopt;
    return value;
}

Json::Value typed(const char* type)
{
    Json::Value message(Json::objectValue);
    message["type"] = type;
    return message;
}

// ---- the moment a snapshot shows ----

struct Seen
{
    unsigned int round = 0;
    unsigned int roundCount = 0;
    unsigned int trick = 0;
    unsigned int trickCount = 0;
    std::size_t onTable = 0;
    std::size_t inHand = 0;
    bool yourTurn = false;
};

Seen seenIn(const Json::Value& snapshot)
{
    Seen seen;
    const Json::Value& round = snapshot["round"];
    seen.round = round["index"].asUInt() + 1;
    seen.roundCount = round["count"].asUInt();
    seen.trick = round["trick"].asUInt();
    seen.trickCount = round["tricks"].asUInt();
    seen.onTable = snapshot["table"]["cards"].size();

    const Json::Value& you = snapshot["you"];
    if(you.isUInt())
    {
        seen.inHand = snapshot["seats"][you.asUInt()]["hand"].size();
        seen.yourTurn = snapshot["active"] == you;
    }
    return seen;
}

// As the desktop's tool words it beside each picture.
std::string describe(const Seen& seen)
{
    return "round " + std::to_string(seen.round) + " of " + std::to_string(seen.roundCount) + ", trick " +
           std::to_string(seen.trick) + " of " + std::to_string(seen.trickCount) + ", " +
           std::to_string(seen.onTable) + " on the table, " + std::to_string(seen.inHand) + " in hand";
}

std::string describe(const Moment& moment)
{
    return "round " + std::to_string(moment.round) + ", trick " + std::to_string(moment.trick) + ", " +
           std::to_string(moment.onTable) + " on the table, " + std::to_string(moment.inHand) + " in hand";
}

// ---- a file ----

Json::Value shotHeader(const std::string& picture, unsigned int width, unsigned int height, const char* view)
{
    Json::Value shot(Json::objectValue);
    shot["file"] = picture;
    shot["width"] = width;
    shot["height"] = height;
    shot["view"] = view;
    return shot;
}

constexpr unsigned int windowWidth = 1920;
constexpr unsigned int windowHeight = 1080;

// The window less its menu bar, which is where the table is drawn at the mock-up's own scale.
constexpr unsigned int tableHeight = 1034;

LookFile makeFile(const std::string& name, Json::Value shot, Json::Value state, std::string moment)
{
    Json::Value content(Json::objectValue);
    content["shot"] = std::move(shot);
    content["state"] = std::move(state);
    return LookFile{name + ".json", std::move(content), std::move(moment)};
}

// A sync as a state: everything a client needs to draw the table, without the envelope.
Json::Value stateOf(Json::Value sync)
{
    sync.removeMember("type");
    sync.removeMember("seq");
    return sync;
}

// ---- one game, played through a session as a browser plays it ----

std::shared_ptr<Service> makeService()
{
    ServiceOptions options;
    options.registry.table.instantFloor = std::chrono::milliseconds(0);

    // As root, a lower niceness than the tool's own would raise the game thread above it.
    options.registry.table.threadNiceness = 19;

    // Table codes are drawn from it, so they are the same on every run.
    options.registry.random = [random = std::mt19937_64(2029)]() mutable { return random(); };
    return std::make_shared<Service>(std::move(options));
}

class Playthrough
{
public:
    Playthrough(const Game& game, std::chrono::milliseconds patience)
        : game_(game),
          patience_(patience),
          service_(makeService()),
          inbox_(std::make_shared<Inbox>()),
          session_(service_, inbox_)
    {
    }

    // Plays to the last picture, or to the first thing that goes wrong.
    Built play()
    {
        try
        {
            run();
        }
        catch(const std::exception& error)
        {
            problem(std::string("stopped: ") + error.what());
        }

        // Every way out leaves the table closed and the game thread joined.
        if(session_.member())
        {
            Json::Value leave = typed("leaveTable");
            session_.receive(toText(leave));
        }
        session_.closed();
        service_->shutdown();
        return std::move(built_);
    }

private:
    void problem(const std::string& what)
    {
        built_.problems.push_back("the " + std::to_string(game_.players) + "-player game: " + what);
    }

    void problem(const Capture& shot, int decision, const std::string& what)
    {
        built_.problems.push_back(std::string(shot.name) + ".png, decision " + std::to_string(decision) + ": " + what);
    }

    void send(const Json::Value& message)
    {
        session_.receive(toText(message));
    }

    // The next message. Nothing, with the problem noted, when the game goes quiet or closes, or
    // the server answers an error.
    std::optional<Json::Value> next()
    {
        const std::optional<std::string> text = inbox_->next(patience_);
        if(!text)
        {
            problem(inbox_->closed() ? "the connection closed" : "no message came in time");
            return std::nullopt;
        }

        std::optional<Json::Value> message = parseMessage(*text);
        if(!message)
        {
            problem("a message that is not JSON");
            return std::nullopt;
        }
        if((*message)["type"] == "error")
        {
            problem("the server answered an error: " + (*message)["code"].asString() + " " +
                    (*message)["detail"].asString());
            return std::nullopt;
        }
        return message;
    }

    void run()
    {
        Json::Value hello = typed("hello");
        hello["protocol"] = protocolVersion;
        hello["lang"] = "en";
        send(hello);

        for(;;)
        {
            const std::optional<Json::Value> message = next();
            if(!message)
                return;
            if((*message)["type"] == "sync")
                break;
        }

        Json::Value create = typed("createSolo");
        create["settings"] = encodeSettings(shotSettings(game_.players));
        send(create);

        std::size_t target = 0;
        int decision = -1;
        while(target < game_.captures.size())
        {
            const std::optional<Json::Value> message = next();
            if(!message)
                return;

            const Json::Value& type = (*message)["type"];
            if(type == "game")
            {
                const std::string kind = (*message)["event"]["kind"].asString();
                if(kind == "gameOver" || kind == "gameStopped")
                {
                    problem(std::string("it ended (") + kind + ") before decision " +
                            std::to_string(game_.captures[target].decisions));
                    return;
                }
                snapshot_ = (*message)["snapshot"];
            }
            else if(type == "prompt")
            {
                // A prompt is shown once. Ids count up, so an older one is not a new decision.
                const std::uint64_t id = (*message)["id"].asUInt64();
                if(id <= lastPrompt_)
                    continue;
                lastPrompt_ = id;
                decision++;

                if(!message->isMember("hand"))
                {
                    problem("a blind prompt at decision " + std::to_string(decision));
                    return;
                }

                const Capture& shot = game_.captures[target];
                if(decision == shot.decisions)
                {
                    if(!capture(shot, decision, *message))
                        return;
                    if(++target == game_.captures.size())
                        return;
                }
                if(!answer(*message, decision))
                    return;
            }
        }
    }

    // The picture's moment, read from the game's last snapshot and the sync the table gives now.
    bool capture(const Capture& shot, int decision, const Json::Value& prompt)
    {
        if(!snapshot_)
        {
            problem(shot, decision, "a prompt came before any game message");
            return false;
        }

        const Seen seen = seenIn(*snapshot_);
        const Moment& wanted = shot.expected;
        if(seen.round != wanted.round || seen.trick != wanted.trick || seen.onTable != wanted.onTable ||
           seen.inHand != wanted.inHand)
        {
            problem(shot, decision, "it shows " + describe(seen) + ", and the desktop's shot is " + describe(wanted));
        }
        if(!seen.yourTurn)
            problem(shot, decision, "the active seat is not the person's");

        const std::optional<MemberId> member = session_.member();
        const std::shared_ptr<Table> table = member ? service_->registry().tableOf(*member) : nullptr;
        if(!table || !table->sync(*member))
        {
            problem(shot, decision, "the table has no sync for the person");
            return false;
        }

        // The game waits on the person's answer, so nothing but the sync can come.
        const std::optional<Json::Value> sync = next();
        if(!sync)
            return false;
        if((*sync)["type"] != "sync")
        {
            problem(shot, decision, "a " + (*sync)["type"].asString() + " message came instead of the sync");
            return false;
        }

        if((*sync)["snapshot"] != *snapshot_)
            problem(shot, decision, "the sync's snapshot differs from the last game message's");

        Json::Value shownPrompt = prompt;
        shownPrompt.removeMember("type");
        shownPrompt.removeMember("seq");
        if((*sync)["prompt"] != shownPrompt)
            problem(shot, decision, "the sync's prompt differs from the prompt sent");
        if((*sync)["game"] != 1)
            problem(shot, decision, "the sync is not of game 1");
        if(!(*sync)["held"].isNull())
            problem(shot, decision, "the game is held");

        built_.files.push_back(makeFile(shot.name,
                                        shotHeader(std::string(shot.name) + ".png", windowWidth, windowHeight, "window"),
                                        stateOf(*sync), describe(seen)));
        return true;
    }

    // Answers as the desktop's player does.
    bool answer(const Json::Value& prompt, int decision)
    {
        Json::Value reply = typed(prompt["kind"] == "bet" ? "bet" : "play");
        reply["prompt"] = prompt["id"];

        if(prompt["kind"] == "bet")
        {
            std::optional<unsigned int> forbidden;
            if(!prompt["forbidden"].isNull())
                forbidden = prompt["forbidden"].asUInt();
            reply["value"] = desktop::lowestBid(prompt["max"].asUInt(), forbidden);
        }
        else
        {
            std::vector<Card> hand;
            for(const Json::Value& code : prompt["hand"])
            {
                const std::optional<Card> card = parseCard(code.asString());
                if(!card)
                {
                    problem("a card the tool cannot read at decision " + std::to_string(decision));
                    return false;
                }
                hand.push_back(*card);
            }

            std::vector<std::size_t> legal;
            for(const Json::Value& index : prompt["legal"])
                legal.push_back(index.asUInt());

            std::optional<Card> trump;
            if(snapshot_)
                trump = parseCard((*snapshot_)["round"]["trump"].asString());
            reply["index"] = static_cast<Json::UInt64>(desktop::firstShownLegal(hand, legal, trump));
        }

        send(reply);
        return true;
    }

    const Game& game_;
    const std::chrono::milliseconds patience_;
    const std::shared_ptr<Service> service_;
    const std::shared_ptr<Inbox> inbox_;
    Session session_;

    // The snapshot of the last game message, which is the table as the person last saw it.
    std::optional<Json::Value> snapshot_;
    std::uint64_t lastPrompt_ = 0;
    Built built_;
};

// ---- the hands, built as the desktop's tool builds them ----

struct HandShot
{
    const char* name;

    // 1-based, of 24: the round of a 4-player game that deals this many cards.
    unsigned int round;
    std::optional<Card> trump;

    // In the order the desktop shows them, those that may follow the lead flagged legal.
    std::vector<Card> hand;
    std::vector<bool> legal;

    // The card under the cursor, as a position in `hand`.
    std::optional<std::size_t> raised;

    // Yours first, then clockwise.
    std::array<unsigned int, 4> bids;
};

const std::vector<HandShot>& handShots()
{
    static const std::vector<HandShot> shots = {
        {"hand-1-card", 1, Card(Rank::Seven, Suit::Clubs),
         {Card(Rank::Jack, Suit::Diamonds)},
         {true}, std::nullopt, {0, 1, 0, 0}},
        {"hand-4-cards", 7, Card(Rank::Seven, Suit::Clubs),
         {Card(Rank::Nine, Suit::Clubs), Card(Rank::Ace, Suit::Spades), Card(Rank::Ten, Suit::Hearts),
          Card(Rank::Eight, Suit::Hearts)},
         {false, false, true, true}, std::nullopt, {1, 2, 0, 1}},
        {"hand-8-cards", 11, std::nullopt,
         {Card(Rank::Ace, Suit::Spades), Card(Rank::Jack, Suit::Spades), Card(Rank::Eight, Suit::Spades),
          Card(Rank::Queen, Suit::Hearts), Card(Rank::Nine, Suit::Hearts), Card(Rank::Seven, Suit::Hearts),
          Card(Rank::Ten, Suit::Clubs), Card(Rank::Queen, Suit::Diamonds)},
         {false, false, false, true, true, true, false, false}, 3, {2, 3, 1, 1}},
    };
    return shots;
}

constexpr std::array<const char*, 4> handNames{"You", "Bogdan", "Ana", "Costel"};

// The catalogue's first four faces go with the names, as in the mock-up.
Profile handProfile(std::size_t seat)
{
    const Catalogue::Avatar& face = Catalogue::get().avatars().at(seat);
    return Profile{handNames[seat], face.gender.value_or("male"), face.id};
}

TableView handTable()
{
    constexpr std::array<Level, 3> levels{Level::LowRisk, Level::Ducking, Level::LowRisk};

    TableView view;
    view.code = "HAND01";
    view.mode = Mode::Solo;
    view.state = TableState::Playing;
    view.host = 0;

    // Without the forehead and hidden rounds, so that a 4-player game has the 24 rounds the shot says.
    view.settings.foreheadAndHidden = false;
    view.settings.botSpeed = fastestSpeed;

    for(std::size_t seat = 0 ; seat < handNames.size() ; seat++)
    {
        SeatSetting setting;
        TableSeatView shown;
        setting.profile = handProfile(seat);
        shown.profile = setting.profile;
        if(seat == 0)
        {
            setting.kind = SeatKind::Human;
            shown.kind = SeatKind::Human;
            shown.you = true;
        }
        else
        {
            setting.kind = SeatKind::Computer;
            setting.level = levels[seat - 1];
            shown.kind = SeatKind::Computer;
            shown.level = setting.level;
        }
        view.settings.seats.push_back(setting);
        view.seats.push_back(shown);
    }
    return view;
}

// The moment the picture shows: nobody has won a trick, every bid is in, and seat 1's King of
// Hearts is on the felt.
romanian_whist::GameSnapshot handSnapshot(const HandShot& shot)
{
    const unsigned int cards = static_cast<unsigned int>(shot.hand.size());

    romanian_whist::GameSnapshot snapshot;
    snapshot.viewer = Seat{0};
    snapshot.status = romanian_whist::GameStatus::InProgress;
    snapshot.phase = romanian_whist::GamePhase::Playing;
    snapshot.roundIndex = shot.round - 1;
    snapshot.roundCount = 24;
    snapshot.trickCount = cards;
    snapshot.trickNumber = 1;
    snapshot.roundType = romanian_whist::RoundType::Normal;
    snapshot.trump = shot.trump;
    snapshot.roundLeader = Seat{1};
    snapshot.activeSeat = Seat{0};
    snapshot.table = {romanian_whist::PlayedCard{Seat{1}, Card(Rank::King, Suit::Hearts)}};
    snapshot.winning = Seat{1};
    snapshot.ledBy = Seat{1};
    snapshot.leadSuit = Suit::Hearts;
    snapshot.betsPlaced = 4;

    for(unsigned int seat = 0 ; seat < handNames.size() ; seat++)
    {
        romanian_whist::SeatSnapshot item;
        item.name = handNames[seat];

        // Seat 1 bid first, as it leads, and the order counts from 1.
        item.biddingOrder = (seat + 3) % 4 + 1;
        item.bet = shot.bids[seat];

        // Seat 1 has played its card.
        item.cardsLeft = seat == 1 ? cards - 1 : cards;
        if(seat == 0)
            item.hand = shot.hand;
        snapshot.seats.push_back(item);
        snapshot.betSum += shot.bids[seat];
    }
    return snapshot;
}

LookFile handFile(const HandShot& shot)
{
    Prompt prompt;
    prompt.id = 1;
    prompt.kind = PromptKind::Play;
    prompt.hand = shot.hand;
    prompt.handSize = shot.hand.size();
    for(std::size_t i = 0 ; i < shot.hand.size() ; i++)
    {
        if(shot.legal[i])
            prompt.legal.push_back(i);
    }

    SyncView sync;
    sync.table = handTable();
    sync.game = 1;
    sync.snapshot = encodeSnapshot(handSnapshot(shot));
    sync.prompt = prompt;

    Json::Value header = shotHeader(std::string(shot.name) + ".png", windowWidth, tableHeight, "table");
    if(shot.raised)
        header["raised"] = cardCode(shot.hand.at(*shot.raised));

    return makeFile(shot.name, std::move(header), stateOf(syncMessage(sync, Clock::now())),
                    "your hand of " + std::to_string(shot.hand.size()));
}

LookFile emptyTable()
{
    return makeFile("table-empty", shotHeader("table-empty.png", windowWidth, tableHeight, "table"),
                    Json::Value(Json::nullValue), "the empty table");
}

} // namespace

Settings shotSettings(unsigned int players)
{
    if(players < 2 || players > opponents.size() + 1)
        throw std::invalid_argument("a game has 2 to 6 players");

    Settings settings;
    std::vector<desktop::Wearer> wearers;

    SeatSetting person;
    person.kind = SeatKind::Human;
    person.profile = Profile{"Player", "male", ""};
    settings.seats.push_back(person);
    wearers.push_back({"male", ""});

    for(unsigned int seat = 1 ; seat < players ; seat++)
    {
        const Opponent& opponent = opponents[seat - 1];

        // The desktop alternates the two, starting with Low risk.
        const Level level = (seat - 1) % 2 == 0 ? Level::LowRisk : Level::Ducking;

        SeatSetting computer;
        computer.kind = SeatKind::Computer;
        computer.level = level;
        computer.profile = Profile{opponent.name, opponent.gender, ""};
        settings.seats.push_back(computer);
        wearers.push_back({opponent.gender, std::string(levelName(level))});
    }

    const std::vector<std::string> faces = desktop::faces(wearers, Catalogue::get().avatars(), dealSeed);
    for(std::size_t seat = 0 ; seat < settings.seats.size() ; seat++)
        settings.seats[seat].profile.avatar = faces[seat];

    settings.structure = romanian_whist::GameStructure::S_181;
    settings.foreheadAndHidden = true;
    settings.allOneCardForehead = false;
    settings.botSpeed = fastestSpeed;
    settings.waitAfterRound = false;
    settings.seed = dealSeed;
    return settings;
}

Built buildLookFiles(std::chrono::milliseconds patience)
{
    Built built;

    for(const Game& game : games())
    {
        Built played = Playthrough(game, patience).play();
        for(LookFile& file : played.files)
            built.files.push_back(std::move(file));
        for(std::string& problem : played.problems)
            built.problems.push_back(std::move(problem));
    }

    for(const HandShot& shot : handShots())
        built.files.push_back(handFile(shot));
    built.files.push_back(emptyTable());
    return built;
}

std::string lookText(const Json::Value& content)
{
    Json::StreamWriterBuilder builder;
    builder["indentation"] = "  ";
    builder["emitUTF8"] = true;
    return Json::writeString(builder, content) + "\n";
}

std::filesystem::path lookDirectory()
{
    return std::filesystem::path(WHIST_LOOK_DIR);
}

} // namespace whist::look
