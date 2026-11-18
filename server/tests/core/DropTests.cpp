#include "support/Online.h"

#include <atomic>
#include <random>

using namespace whist;
using std::chrono::milliseconds;
using test::Match;
using test::Online;
using test::ofType;
using test::onlineSettings;
using test::patience;

namespace
{

// Long enough for a human thread to have reacted to a message, and short enough to keep tests quick.
const milliseconds grace(400 * test::slowdown);

// Longer than any test waits for a turn, unless it means the timer to run out.
const milliseconds longTurn(60000 * test::slowdown);

struct Drops : Online
{
    explicit Drops(milliseconds gracePeriod = grace, milliseconds turnTimer = longTurn,
                   milliseconds idle = std::chrono::minutes(10), milliseconds pongTimeout = milliseconds(30000))
        : Online(milliseconds(30000), [=](ServiceOptions& options)
        {
            options.registry.table.gracePeriod = gracePeriod;
            options.registry.table.turnTimerOverride = turnTimer;
            options.registry.table.onlineIdle = idle;
            options.pongTimeout = pongTimeout;
        })
    {
    }

    // Ana hosts a table of three seats with Bea, and a computer player in the third.
    void seat()
    {
        open(1);
        open(2);
        Settings settings = onlineSettings(3, 2);
        settings.foreheadAndHidden = false;
        create(1, settings, "Ana");
        ask(2, join("Bea"), ofType("sync"));
    }

    std::shared_ptr<Table> theTable() const
    {
        return service->registry().findTable(code);
    }

    std::optional<Json::Value> lastOf(MemberId label, const std::string& type) const
    {
        const std::vector<Json::Value> messages = recorder.messagesTo(label);
        for(auto it = messages.rbegin() ; it != messages.rend() ; ++it)
        {
            if((*it)["type"] == type)
                return *it;
        }
        return std::nullopt;
    }

    std::string presence(MemberId observer, unsigned int seat) const
    {
        return table(observer)["seats"][seat]["presence"].asString();
    }
};

template <class Predicate>
bool eventually(Predicate holds, milliseconds timeout = patience)
{
    const auto end = Clock::now() + timeout;
    while(!holds())
    {
        if(Clock::now() >= end)
            return false;
        std::this_thread::sleep_for(milliseconds(2));
    }
    return true;
}

// What a good player would answer to a prompt.
Json::Value answerTo(const Json::Value& prompt)
{
    const bool bet = prompt["kind"] == "bet";
    Json::Value message = test::typed(bet ? "bet" : "play");
    message["prompt"] = prompt["id"];
    if(bet)
    {
        const bool zeroForbidden = prompt["forbidden"].isUInt() && prompt["forbidden"].asUInt() == 0;
        message["value"] = zeroForbidden ? 1 : 0;
    }
    else
        message["index"] = prompt["legal"][0];
    return message;
}

// Answers its first `answerFirst` prompts like any good player, then lets every prompt wait until it
// is released. A game that waits on someone cannot run to its end while a test is still acting.
struct Holder
{
    explicit Holder(unsigned int answerFirst) : answerFirst_(answerFirst) {}

    std::atomic<bool> release{false};

    test::ScriptedHuman::OnPrompt hook()
    {
        return [this](const Json::Value&) { return seen_.fetch_add(1) >= answerFirst_ && !release.load(); };
    }

private:
    const unsigned int answerFirst_;
    std::atomic<unsigned int> seen_{0};
};

bool hasWaitingFor(const Json::Value& message)
{
    return message["type"] == "tableState" && message.isMember("waitingFor");
}

bool isReplaced(const Json::Value& message, unsigned int seat)
{
    return message["type"] == "tableState" && message["seats"][seat]["presence"] == "replaced";
}

} // namespace

TEST_CASE("Timeline 1: a reload inside the grace period; the table waits, and she answers", "[drops][timeline]")
{
    Drops d(milliseconds(5000 * test::slowdown));
    d.seat();

    test::ScriptedHuman ana(d.recorder, 1, d.answerer(1), test::Behaviour::Good);
    test::ScriptedHuman bea(d.recorder, 2, d.answerer(2), test::Behaviour::Silent);
    d.send(1, test::typed("startGame"));
    REQUIRE(bea.waitForPrompt(patience));
    const Json::Value prompt = *d.lastOf(2, "prompt");

    // Everyone sees her go, and that her turn is being waited for.
    std::size_t before = d.mark(1);
    d.drop(2);
    const auto waiting = d.waitFrom(1, before, hasWaitingFor);
    REQUIRE(waiting);
    CHECK((*waiting)["seats"][1]["presence"] == "away");
    CHECK((*waiting)["waitingFor"]["seat"] == 1);
    CHECK((*waiting)["waitingFor"]["deadline"].asInt64() > 0);
    CHECK((*waiting)["waitingFor"]["deadline"].asInt64() <= 5000 * test::slowdown);
    CHECK(test::shapeError(*waiting).empty());

    // Back in time: her sync carries the prompt she had, and the others see her here again.
    before = d.mark(1);
    d.reopen(2);
    const Json::Value sync = *d.lastOf(2, "sync");
    CHECK(sync["prompt"]["id"] == prompt["id"]);
    CHECK(sync["table"]["seats"][1]["presence"] == "here");
    CHECK(sync["table"]["seats"][1]["you"] == true);
    const auto back = d.waitFrom(1, before, ofType("tableState"));
    REQUIRE(back);
    CHECK((*back)["seats"][1]["presence"] == "here");
    CHECK_FALSE(back->isMember("waitingFor"));

    // She answers, and the answer is taken.
    before = d.mark(2);
    d.send(2, answerTo(sync["prompt"]));
    const auto closed = d.waitFrom(2, before, ofType("promptClosed"));
    REQUIRE(closed);
    CHECK((*closed)["by"] == "you");

    // Nobody was ever replaced.
    for(const Json::Value& message : d.recorder.messagesTo(1))
        CHECK_FALSE(isReplaced(message, 1));
}

TEST_CASE("Timeline 2: gone for good during someone else's turn; replaced once the grace period is over",
          "[drops][timeline]")
{
    Drops d;
    d.seat();

    // Ana's first prompt waits, so Bea's turn has not come when she goes.
    Holder hold(0);
    test::ScriptedHuman ana(d.recorder, 1, d.answerer(1), test::Behaviour::Good, hold.hook());
    test::ScriptedHuman bea(d.recorder, 2, d.answerer(2), test::Behaviour::Good);
    d.send(1, test::typed("startGame"));
    REQUIRE(ana.waitForPrompt(patience));
    REQUIRE(bea.counts().prompts == 0);
    const Json::Value held = *d.lastOf(1, "prompt");

    d.drop(2);
    REQUIRE(eventually([&] { return d.presence(1, 1) == "away"; }));
    CHECK_FALSE(d.table(1).isMember("waitingFor"));

    // The grace period passes with nothing to notice it but the reaper.
    d.service->reap(Clock::now() + grace * 2);
    REQUIRE(eventually([&] { return d.presence(1, 1) == "replaced"; }));

    // The game goes on, with her computer player making her moves, to the end.
    hold.release = true;
    d.send(1, answerTo(held));
    REQUIRE(ana.waitUntilOver(patience));
    CHECK(ana.problems().empty());

    // The others were told of her computer player at the table: her seat is hers on the table.
    CHECK(d.table(1)["seats"][1]["name"] == "Bea");
}

TEST_CASE("Timeline 3: gone during her own turn; the table waits for what is left of the grace period",
          "[drops][timeline]")
{
    Drops d;
    d.seat();

    test::ScriptedHuman ana(d.recorder, 1, d.answerer(1), test::Behaviour::Good);
    test::ScriptedHuman bea(d.recorder, 2, d.answerer(2), test::Behaviour::Silent);
    d.send(1, test::typed("startGame"));
    REQUIRE(bea.waitForPrompt(patience));

    const std::size_t before = d.mark(1);
    d.drop(2);
    const auto waiting = d.waitFrom(1, before, hasWaitingFor);
    REQUIRE(waiting);
    CHECK((*waiting)["waitingFor"]["seat"] == 1);
    CHECK((*waiting)["waitingFor"]["deadline"].asInt64() <= grace.count());

    // Then her computer player moves, and the seat is replaced.
    REQUIRE(d.waitFrom(1, before, [](const Json::Value& m) { return isReplaced(m, 1); }));
    const Json::Value state = d.table(1);
    CHECK_FALSE(state.isMember("waitingFor"));
    REQUIRE(ana.waitUntilOver(patience));
    CHECK(ana.problems().empty());
}

TEST_CASE("Timeline 4: here but not playing; three turns in a row run out, and `back` takes the seat back",
          "[drops][timeline]")
{
    Drops d(grace, milliseconds(150 * test::slowdown));
    d.seat();

    Holder hold(5);
    test::ScriptedHuman ana(d.recorder, 1, d.answerer(1), test::Behaviour::Good, hold.hook());
    test::ScriptedHuman bea(d.recorder, 2, d.answerer(2), test::Behaviour::Silent);
    d.send(1, test::typed("startGame"));

    // Each turn that runs out is made for her; the third replaces her, and she is told.
    const auto replaced = d.recorder.waitFor(2, [](const Json::Value& m) { return isReplaced(m, 1); }, patience);
    REQUIRE(replaced);
    CHECK((*replaced)["seats"][1]["you"] == true);
    REQUIRE(eventually([&] { return bea.counts().closedByComputer == 3; }));
    CHECK(bea.counts().closedByYou == 0);
    const std::size_t prompts = bea.counts().prompts;

    // `back` is just that: her seat is hers again, and she is asked at her next turn.
    const std::size_t before = d.mark(2);
    d.send(2, test::typed("back"));
    const auto here = d.waitFrom(2, before, ofType("tableState"));
    REQUIRE(here);
    CHECK((*here)["seats"][1]["presence"] == "here");
    CHECK(d.presence(1, 1) == "here");

    hold.release = true;
    d.send(1, answerTo(*d.lastOf(1, "prompt")));
    REQUIRE(eventually([&] { return bea.counts().prompts > prompts; }));
}

TEST_CASE("Timeline 4: anything else she sends takes the seat back too, and `back` with nothing to take back does nothing",
          "[drops][timeline]")
{
    Drops d(grace, milliseconds(150 * test::slowdown));
    d.seat();

    Holder hold(5);
    test::ScriptedHuman ana(d.recorder, 1, d.answerer(1), test::Behaviour::Good, hold.hook());
    test::ScriptedHuman bea(d.recorder, 2, d.answerer(2), test::Behaviour::Silent);

    // Nothing to take back yet: no answer, no message.
    const std::size_t quietBefore = d.mark(2);
    d.send(2, test::typed("back"));
    CHECK(d.mark(2) == quietBefore);

    d.send(1, test::typed("startGame"));
    REQUIRE(d.recorder.waitFor(2, [](const Json::Value& m) { return isReplaced(m, 1); }, patience));

    const std::size_t before = d.mark(2);
    Json::Value anything = test::typed("continue");
    anything["round"] = 0;
    d.send(2, anything);
    const auto here = d.waitFrom(2, before, ofType("tableState"));
    REQUIRE(here);
    CHECK((*here)["seats"][1]["presence"] == "here");

    // Taking back a seat that is hers does nothing and says nothing.
    const std::size_t after = d.mark(2);
    d.send(2, test::typed("back"));
    std::this_thread::sleep_for(milliseconds(50 * test::slowdown));
    for(std::size_t i = after ; i < d.mark(2) ; i++)
        CHECK(d.recorder.messagesTo(2)[i]["type"] != "error");
}

TEST_CASE("Timeline 5: leaving on purpose replaces her at once, and nothing more of the game reaches her",
          "[drops][timeline]")
{
    Drops d;
    d.seat();

    // Ana's second prompt waits, so the game is still on when Bea leaves.
    Holder hold(1);
    test::ScriptedHuman ana(d.recorder, 1, d.answerer(1), test::Behaviour::Good, hold.hook());
    test::ScriptedHuman bea(d.recorder, 2, d.answerer(2), test::Behaviour::Good);
    d.send(1, test::typed("startGame"));
    REQUIRE(eventually([&] { return ana.counts().prompts >= 2; }));

    // She is told she is at no table, and her seat is a computer player's from now on.
    const Json::Value left = d.ask(2, test::typed("leaveTable"), ofType("sync"));
    CHECK(left["table"].isNull());
    const std::size_t sent = d.mark(2);
    REQUIRE(eventually([&] { return d.presence(1, 1) == "replaced"; }));
    CHECK(d.table(1)["state"] == "playing");
    CHECK_FALSE(d.service->registry().tableOf(*d.tab(2).member()));

    // The game goes on to its end without her, and none of it reaches her.
    hold.release = true;
    d.send(1, answerTo(*d.lastOf(1, "prompt")));
    REQUIRE(ana.waitUntilOver(patience));
    CHECK(ana.problems().empty());
    CHECK(d.mark(2) == sent);

    // Her prompts, if any, were closed without a word, and she is free to start another table.
    Json::Value solo = test::typed("createSolo");
    solo["settings"] = encodeSettings(test::tableSettings(2, {0}));
    CHECK(d.ask(2, solo, ofType("tableState"))["mode"] == "solo");
}

TEST_CASE("Timeline 5: the same link brings her back to her seat", "[drops][timeline]")
{
    Drops d;
    d.seat();

    Holder hold(1);
    test::ScriptedHuman ana(d.recorder, 1, d.answerer(1), test::Behaviour::Good, hold.hook());
    test::ScriptedHuman bea(d.recorder, 2, d.answerer(2), test::Behaviour::Good);
    d.send(1, test::typed("startGame"));
    REQUIRE(eventually([&] { return ana.counts().prompts >= 2; }));

    d.ask(2, test::typed("leaveTable"), ofType("sync"));
    REQUIRE(eventually([&] { return d.presence(1, 1) == "replaced"; }));

    // The profile she brings is not used: the engine fixed the names at the start.
    const Json::Value back = d.ask(2, d.join("Someone Else"), ofType("sync"));
    CHECK(back["table"]["seats"][1]["you"] == true);
    CHECK(back["table"]["seats"][1]["name"] == "Bea");
    CHECK(back["snapshot"]["you"] == 1);
    CHECK(back["game"] == 1);
    REQUIRE(eventually([&] { return d.presence(1, 1) == "here"; }));

    // Anyone else is still turned away.
    d.open(3);
    CHECK(d.refusal(3, d.join("Cora"))["code"] == "tableFull");

    hold.release = true;
    d.send(1, answerTo(*d.lastOf(1, "prompt")));
    REQUIRE(ana.waitUntilOver(patience));
    REQUIRE(bea.waitUntilOver(patience));
    CHECK(ana.problems().empty());
    CHECK(bea.problems().empty());
}

TEST_CASE("Timeline 6: coming back after being replaced; the seat is hers from the next decision", "[drops][timeline]")
{
    Drops d;
    d.seat();

    // After her first prompt, Ana waits at each, so the game stands still while Bea is away.
    Holder hold(1);
    test::ScriptedHuman ana(d.recorder, 1, d.answerer(1), test::Behaviour::Good, hold.hook());
    test::ScriptedHuman bea(d.recorder, 2, d.answerer(2), test::Behaviour::Silent);
    d.send(1, test::typed("startGame"));
    REQUIRE(bea.waitForPrompt(patience));

    d.drop(2);
    REQUIRE(d.waitFrom(1, 0, [](const Json::Value& m) { return isReplaced(m, 1); }));
    REQUIRE(eventually([&] { return ana.counts().prompts >= 2; }));

    const std::size_t prompts = bea.counts().prompts;
    d.reopen(2);
    const Json::Value sync = *d.lastOf(2, "sync");
    CHECK(sync["table"]["seats"][1]["presence"] == "here");
    CHECK(sync["table"]["seats"][1]["you"] == true);
    CHECK(sync["prompt"].isNull());
    REQUIRE(eventually([&] { return d.presence(1, 1) == "here"; }));

    // The game goes on, and she is asked again.
    hold.release = true;
    d.send(1, answerTo(*d.lastOf(1, "prompt")));
    REQUIRE(eventually([&] { return bea.counts().prompts > prompts; }));
}

TEST_CASE("A host who goes away in the lobby hands over when the grace period ends", "[drops][host]")
{
    Drops d;
    d.seat();
    CHECK(d.table(2)["host"] == 0);

    d.drop(1);
    REQUIRE(eventually([&] { return d.presence(2, 0) == "away"; }));
    CHECK(d.table(2)["host"] == 0);

    d.service->reap(Clock::now() + grace * 2);
    REQUIRE(eventually([&] { return d.table(2)["host"] == 1; }));

    // Back, she is a guest, and the new host can start the game.
    d.reopen(1);
    const Json::Value sync = *d.lastOf(1, "sync");
    CHECK(sync["table"]["host"] == 1);
    CHECK(sync["table"]["seats"][0]["you"] == true);
    CHECK(d.refusal(1, test::typed("startGame"))["code"] == "notHost");
}

TEST_CASE("A game nobody is connected to holds; one person returns and it goes on", "[drops][hold]")
{
    Drops d;
    d.seat();

    Holder hold(1);
    test::ScriptedHuman ana(d.recorder, 1, d.answerer(1), test::Behaviour::Good, hold.hook());
    test::ScriptedHuman bea(d.recorder, 2, d.answerer(2), test::Behaviour::Good);
    d.send(1, test::typed("startGame"));
    REQUIRE(eventually([&] { return ana.counts().prompts >= 2; }));

    d.drop(1);
    d.drop(2);

    // Her prompt runs out of grace, the computer players play on until the next rest, and there the
    // game holds.
    const auto table = d.theTable();
    REQUIRE(table);
    std::this_thread::sleep_for(grace + milliseconds(300 * test::slowdown));
    const std::size_t rounds = table->scoredRounds();
    std::this_thread::sleep_for(milliseconds(300 * test::slowdown));
    CHECK(table->scoredRounds() == rounds);
    CHECK(table->outcome() == RunOutcome::Running);

    // Someone returns, and the game goes on to its end.
    hold.release = true;
    d.reopen(1);
    REQUIRE(ana.waitUntilOver(patience));
    CHECK(table->outcome() == RunOutcome::Finished);
}

TEST_CASE("A table nobody returns to closes, and its game's thread is joined at once", "[drops][hold][close]")
{
    Drops d(grace, longTurn, std::chrono::minutes(10));
    d.seat();

    Holder hold(1);
    test::ScriptedHuman ana(d.recorder, 1, d.answerer(1), test::Behaviour::Good, hold.hook());
    test::ScriptedHuman bea(d.recorder, 2, d.answerer(2), test::Behaviour::Good);
    d.send(1, test::typed("startGame"));
    REQUIRE(eventually([&] { return ana.counts().prompts >= 2; }));

    d.drop(1);
    d.drop(2);
    std::this_thread::sleep_for(grace + milliseconds(300 * test::slowdown));

    // Not yet: ten minutes of nobody being there have not passed.
    d.service->reap(Clock::now() + std::chrono::minutes(5));
    CHECK(d.theTable());

    const std::size_t joined = d.service->registry().joiner().joined();
    d.service->reap(Clock::now() + std::chrono::minutes(11));
    CHECK_FALSE(d.theTable());
    CHECK(eventually([&] { return d.service->registry().joiner().joined() > joined; }, milliseconds(1000 * test::slowdown)));
}

TEST_CASE("Closing a table while a seat waits for an away member joins at once", "[drops][close]")
{
    Drops d;
    d.seat();

    test::ScriptedHuman ana(d.recorder, 1, d.answerer(1), test::Behaviour::Good);
    test::ScriptedHuman bea(d.recorder, 2, d.answerer(2), test::Behaviour::Silent);
    d.send(1, test::typed("startGame"));
    REQUIRE(bea.waitForPrompt(patience));
    d.drop(2);
    REQUIRE(eventually([&] { return d.presence(1, 1) == "away"; }));

    const std::size_t joined = d.service->registry().joiner().joined();
    d.service->registry().closeTable(d.code);
    CHECK(eventually([&] { return d.service->registry().joiner().joined() > joined; }, milliseconds(1000 * test::slowdown)));
}

TEST_CASE("A connection that says nothing for the pong timeout is closed, and its member is away", "[drops][pong]")
{
    const milliseconds timeout(400 * test::slowdown);
    Drops d(grace, longTurn, std::chrono::minutes(10), timeout);
    d.seat();

    // A tab that has never said hello is silent too, and goes with them.
    test::Client stray(d.service, d.recorder, 50);

    std::this_thread::sleep_for(timeout + milliseconds(100 * test::slowdown));

    // Ana's tab is heard from, Bea's is not.
    d.tab(1).heard();
    d.service->reap(Clock::now());

    CHECK(d.recorder.closedAfter(2));
    CHECK(d.recorder.closedAfter(50));
    CHECK_FALSE(d.recorder.closedAfter(1));
    REQUIRE(eventually([&] { return d.presence(1, 1) == "away"; }));
    CHECK(d.presence(1, 0) == "here");

    // Her tab's own close afterwards says nothing more.
    const std::size_t before = d.mark(1);
    d.drop(2);
    std::this_thread::sleep_for(milliseconds(100 * test::slowdown));
    CHECK(d.mark(1) == before);
}

TEST_CASE("A member nobody has been connected as for long is forgotten; one connected, or at a table, is not",
          "[drops][reaper]")
{
    using std::chrono::minutes;
    Drops d;
    d.open(1);
    d.open(2);
    d.open(3);
    Settings settings = onlineSettings(3, 2);
    d.create(2, settings, "Ana");
    d.drop(1);
    d.drop(2);

    Registry& registry = d.service->registry();
    const auto known = [&](MemberId label) { return registry.findMember(d.tokens.at(label)).has_value(); };

    // The clock starts at the first look. Member 1 is at no table; member 2's table closes for being
    // idle, and only then does their own hour begin.
    const Clock::time_point now = Clock::now();
    d.service->reap(now);
    d.service->reap(now + minutes(59));
    CHECK(known(1));
    CHECK(known(2));
    CHECK_FALSE(d.theTable());

    d.tab(3).heard();
    d.service->reap(now + minutes(61));
    CHECK_FALSE(known(1));
    CHECK(known(2));

    d.service->reap(now + minutes(121));
    CHECK_FALSE(known(2));
}

namespace
{

// People who come and go at random while a game is played: each tab is closed and opened again, or
// its player leaves and joins again by the table's code, or takes a seat back.
void churn(Drops& d, const std::vector<MemberId>& labels, std::atomic<bool>& stop, unsigned int seed)
{
    std::mt19937 random(seed);
    while(!stop)
    {
        const MemberId label = labels[random() % labels.size()];
        switch(random() % 5)
        {
            case 0:
                d.tab(label).drop();
                d.reconnect(label);
                break;
            case 1:
                d.tab(label).drop();
                std::this_thread::sleep_for(milliseconds(random() % 40));
                d.reconnect(label);
                break;
            case 2:
                d.send(label, test::typed("leaveTable"));
                std::this_thread::sleep_for(milliseconds(random() % 20));
                d.send(label, d.join("Back" + std::to_string(label)));
                break;
            case 3:
                d.send(label, test::typed("back"));
                break;
            default:
                break;
        }
        std::this_thread::sleep_for(milliseconds(random() % 15));
    }
}

} // namespace

TEST_CASE("People who come and go at random never stop a game, and every message is well formed",
          "[drops][churn]")
{
    // Short grace and turn timers, so people are replaced and take their seats back all game long.
    Drops d(milliseconds(60 * test::slowdown), milliseconds(80 * test::slowdown));
    for(MemberId label : {1, 2, 3})
        d.open(label);
    Settings settings = onlineSettings(6, 4);
    settings.structure = romanian_whist::GameStructure::S_181;
    d.create(1, settings, "Ana");
    d.ask(2, d.join("Bea"), ofType("sync"));
    d.ask(3, d.join("Cora"), ofType("sync"));
    d.ask(1, d.setSeat(3, Level::ReckonerAlpha), ofType("tableState"));

    std::vector<std::unique_ptr<test::ScriptedHuman>> humans;
    for(MemberId label : {1, 2, 3})
    {
        humans.push_back(std::make_unique<test::ScriptedHuman>(d.recorder, label, d.answerer(label),
                                                               test::Behaviour::Good, test::ScriptedHuman::OnPrompt{},
                                                               d.continuer(label)));
    }
    d.send(1, test::typed("startGame"));

    std::atomic<bool> stop{false};
    std::thread comers(churn, std::ref(d), std::vector<MemberId>{2, 3}, std::ref(stop), 17);

    // Ana never leaves, so she sees the game to its end, however the others come and go.
    const bool over = humans[0]->waitUntilOver(patience);
    stop = true;
    comers.join();
    REQUIRE(over);
    CHECK(humans[0]->problems().empty());

    for(MemberId label : {1, 2, 3})
        test::checkShapes(d.recorder.messagesTo(label));
}
