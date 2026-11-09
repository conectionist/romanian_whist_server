#include "core/GameAbandoned.h"
#include "core/SeatDriver.h"

#include "support/Recorder.h"
#include "support/ServerMessages.h"

#include <catch2/catch_test_macros.hpp>

#include <future>
#include <random>
#include <thread>

using namespace whist;
using namespace romanian_whist;
using std::chrono::milliseconds;

namespace
{

constexpr MemberId ana = 1;
const milliseconds patience(5000 * test::slowdown);

struct Seat0
{
    test::Recorder recorder;
    Outbox outbox;
    std::vector<Card> hand{Card{Rank::Ace, Suit::Hearts}, Card{Rank::Ten, Suit::Spades}, Card{Rank::Four, Suit::Clubs}};
    std::vector<Card> played{Card{Rank::King, Suit::Spades}};
    std::unique_ptr<SeatDriver> driver;
    std::size_t cursor = 0;

    // How often the driver told the table something had changed.
    std::atomic<int> changes{0};

    explicit Seat0(std::optional<milliseconds> turnTimer = std::nullopt)
    {
        outbox.attach(ana, recorder.connect(ana));
        driver = std::make_unique<SeatDriver>(Seat{0}, ana, makeComputerPlayer(Level::LowRisk, Seat{0}, std::nullopt),
                                              turnTimer, outbox, std::make_shared<std::atomic<std::uint64_t>>(0),
                                              [this] { changes++; });
    }

    // Asks for a bid on another thread, as the engine would on the game thread.
    std::future<unsigned int> bid(std::optional<unsigned int> forbidden = 1, bool blind = false)
    {
        return std::async(std::launch::async, [this, forbidden, blind]
        {
            BetContext context{hand};
            context.forbiddenBet = forbidden;
            return driver->makeBet(context, blind);
        });
    }

    // A spade was led, so only the ten of spades is legal.
    std::future<std::optional<std::size_t>> play(bool blind = false)
    {
        return std::async(std::launch::async, [this, blind]
        {
            PlayContext context{hand, played};
            context.leadSuit = Suit::Spades;
            return driver->playCard(context, blind);
        });
    }

    Json::Value next()
    {
        std::optional<Json::Value> message = recorder.next(ana, cursor, patience);
        REQUIRE(message);
        INFO(test::shapeError(*message));
        CHECK(test::shapeError(*message).empty());
        return *message;
    }

    Answer answer(PromptKind kind, std::uint64_t prompt, std::uint64_t value)
    {
        return Answer{kind, prompt, value};
    }
};

} // namespace

TEST_CASE("A bid prompt carries everything its answer needs", "[seat]")
{
    Seat0 seat;
    auto bid = seat.bid();

    const Json::Value prompt = seat.next();
    CHECK(prompt["type"] == "prompt");
    CHECK(prompt["kind"] == "bet");
    CHECK(prompt["id"] == 1);
    CHECK(prompt["max"] == 3);
    CHECK(prompt["forbidden"] == 1);
    CHECK(prompt["deadline"].isNull());
    CHECK(prompt["hand"].size() == 3);
    CHECK(prompt["hand"][1] == "TS");

    CHECK(seat.driver->submit(seat.answer(PromptKind::Bet, 1, 2)) == SeatDriver::Submitted::Taken);
    CHECK(bid.get() == 2);
    const Json::Value closed = seat.next();
    CHECK(closed["type"] == "promptClosed");
    CHECK(closed["by"] == "you");
}

TEST_CASE("A play prompt names the legal cards by their place in the hand", "[seat]")
{
    Seat0 seat;
    auto play = seat.play();

    const Json::Value prompt = seat.next();
    CHECK(prompt["kind"] == "play");
    REQUIRE(prompt["legal"].size() == 1);
    CHECK(prompt["legal"][0] == 1);
    CHECK(prompt["blind"] == false);

    seat.driver->submit(seat.answer(PromptKind::Play, 1, 1));
    CHECK(play.get() == 1u);
}

TEST_CASE("A blind prompt carries no hand", "[seat]")
{
    Seat0 seat;
    seat.hand = {Card{Rank::Ace, Suit::Hearts}};
    auto bid = seat.bid(std::nullopt, true);

    const Json::Value prompt = seat.next();
    CHECK_FALSE(prompt.isMember("hand"));
    CHECK(prompt["handSize"] == 1);
    seat.driver->submit(seat.answer(PromptKind::Bet, 1, 0));
    CHECK(bid.get() == 0);
}

TEST_CASE("Stale, doubled and early answers are refused at the gate", "[seat]")
{
    Seat0 seat;
    CHECK(seat.driver->submit(seat.answer(PromptKind::Bet, 1, 0)) == SeatDriver::Submitted::NotYourTurn);

    auto bid = seat.bid();
    const Json::Value prompt = seat.next();
    CHECK(seat.driver->submit(seat.answer(PromptKind::Bet, 7, 0)) == SeatDriver::Submitted::Stale);
    CHECK(seat.driver->submit(seat.answer(PromptKind::Bet, 1, 0)) == SeatDriver::Submitted::Taken);
    CHECK(bid.get() == 0);

    // The same answer again, once its prompt has closed.
    CHECK(seat.driver->submit(seat.answer(PromptKind::Bet, 1, 0)) == SeatDriver::Submitted::Stale);
}

TEST_CASE("An invalid answer is refused, and the same prompt asked again", "[seat]")
{
    Seat0 seat;
    auto bid = seat.bid();
    seat.next();

    struct Case
    {
        PromptKind kind;
        std::uint64_t value;
    };

    // The forbidden bid, one above the hand, one that would wrap to a legal bid, and a play.
    for(const Case wrong : {Case{PromptKind::Bet, 1}, Case{PromptKind::Bet, 4}, Case{PromptKind::Bet, 4294967297ULL},
                            Case{PromptKind::Play, 0}})
    {
        INFO(wrong.value);
        CHECK(seat.driver->submit(seat.answer(wrong.kind, 1, wrong.value)) == SeatDriver::Submitted::Taken);
        const Json::Value error = seat.next();
        CHECK(error["type"] == "error");
        CHECK(error["code"] == "illegal");
        const Json::Value again = seat.next();
        CHECK(again["type"] == "prompt");
        CHECK(again["id"] == 1);
    }

    seat.driver->submit(seat.answer(PromptKind::Bet, 1, 3));
    CHECK(bid.get() == 3);
}

TEST_CASE("An illegal card is refused, and the same prompt asked again", "[seat]")
{
    Seat0 seat;
    auto play = seat.play();
    seat.next();

    for(std::uint64_t index : {0ULL, 2ULL, 3ULL, 1ULL << 40})
    {
        seat.driver->submit(seat.answer(PromptKind::Play, 1, index));
        CHECK(seat.next()["code"] == "illegal");
        CHECK(seat.next()["id"] == 1);
    }

    seat.driver->submit(seat.answer(PromptKind::Bet, 1, 1));
    CHECK(seat.next()["code"] == "illegal");
    CHECK(seat.next()["id"] == 1);

    seat.driver->submit(seat.answer(PromptKind::Play, 1, 1));
    CHECK(play.get() == 1u);
}

TEST_CASE("Abandoning a seat ends its wait, before or during it", "[seat]")
{
    SECTION("during")
    {
        Seat0 seat;
        auto bid = seat.bid();
        seat.next();
        seat.driver->abandon();
        CHECK_THROWS_AS(bid.get(), GameAbandoned);
        CHECK(seat.driver->submit(seat.answer(PromptKind::Bet, 1, 0)) == SeatDriver::Submitted::Stale);
    }

    SECTION("before")
    {
        Seat0 seat;
        seat.driver->abandon();
        auto bid = seat.bid();
        CHECK_THROWS_AS(bid.get(), GameAbandoned);
        CHECK(seat.recorder.messagesTo(ana).empty());
    }
}

TEST_CASE("When the turn timer runs out, the computer player moves", "[seat]")
{
    Seat0 seat(milliseconds(20));
    auto play = seat.play();

    const Json::Value prompt = seat.next();
    CHECK(prompt["deadline"].asInt() > 0);
    CHECK(prompt["deadline"].asInt() <= 20);

    // Only the ten of spades follows suit.
    CHECK(play.get() == 1u);
    const Json::Value closed = seat.next();
    CHECK(closed["type"] == "promptClosed");
    CHECK(closed["by"] == "computer");
    CHECK(seat.driver->submit(seat.answer(PromptKind::Play, 1, 1)) == SeatDriver::Submitted::Stale);
}

TEST_CASE("Nonsense for ever is answered by the computer player", "[seat]")
{
    Seat0 seat(milliseconds(50));
    auto bid = seat.bid();
    seat.next();

    std::atomic<bool> done = false;
    std::thread nonsense([&]
    {
        while(!done)
        {
            seat.driver->submit(seat.answer(PromptKind::Bet, 1, 1));
            std::this_thread::sleep_for(milliseconds(1));
        }
    });

    const unsigned int made = bid.get();
    done = true;
    nonsense.join();
    CHECK(made != 1);
    const auto messages = seat.recorder.messagesTo(ana);
    CHECK(messages.back()["by"] == "computer");
}

TEST_CASE("An answer and a deadline at the same moment: exactly one wins", "[seat][race]")
{
    std::mt19937 random(11);
    int answers = 0;
    int timeouts = 0;

    for(int round = 0 ; round < 300 ; round++)
    {
        Seat0 seat(milliseconds(2));
        auto bid = seat.bid();
        const Json::Value prompt = seat.next();

        std::this_thread::sleep_for(std::chrono::microseconds(random() % 4000));
        const SeatDriver::Submitted submitted = seat.driver->submit(seat.answer(PromptKind::Bet, prompt["id"].asUInt64(), 3));
        const unsigned int made = bid.get();
        const Json::Value closed = seat.next();
        REQUIRE(closed["type"] == "promptClosed");

        if(submitted == SeatDriver::Submitted::Taken)
        {
            answers++;
            CHECK(closed["by"] == "you");
            CHECK(made == 3);
        }
        else
        {
            timeouts++;
            CHECK(submitted == SeatDriver::Submitted::Stale);
            CHECK(closed["by"] == "computer");
            CHECK(made != 1);
        }
        CHECK(seat.recorder.messagesTo(ana).size() == 2);
    }

    // Both sides of the race were reached.
    CHECK(answers > 0);
    CHECK(timeouts > 0);
}

TEST_CASE("A member who is away is waited for until the grace period ends, then replaced", "[seat][away]")
{
    Seat0 seat(milliseconds(60000));
    seat.driver->away(Clock::now() + milliseconds(200 * test::slowdown));
    auto play = seat.play();

    const Json::Value prompt = seat.next();
    CHECK(prompt["deadline"].asInt() > 50000);
    CHECK(seat.driver->awayDeadline());

    // Only the ten of spades follows suit.
    CHECK(play.get() == 1u);
    const Json::Value closed = seat.next();
    CHECK(closed["by"] == "computer");
    CHECK(seat.driver->replaced());
    CHECK_FALSE(seat.driver->humanInCharge());
    CHECK_FALSE(seat.driver->awayDeadline());

    // The table was told the prompt opened for an away member, and that the seat was replaced.
    CHECK(seat.changes == 2);
}

TEST_CASE("A member who is back before the grace period ends answers as usual", "[seat][away]")
{
    Seat0 seat(milliseconds(60000));
    seat.driver->away(Clock::now() + milliseconds(30000));
    auto bid = seat.bid();
    const Json::Value prompt = seat.next();

    seat.driver->here();
    CHECK_FALSE(seat.driver->awayDeadline());
    CHECK(seat.driver->submit(seat.answer(PromptKind::Bet, prompt["id"].asUInt64(), 3)) == SeatDriver::Submitted::Taken);
    CHECK(bid.get() == 3u);
    CHECK(seat.next()["by"] == "you");
    CHECK_FALSE(seat.driver->replaced());
}

TEST_CASE("An away member's wait ends at the turn timer when that comes first", "[seat][away]")
{
    Seat0 seat(milliseconds(30));
    seat.driver->away(Clock::now() + milliseconds(60000));
    auto play = seat.play();
    seat.next();

    CHECK(play.get() == 1u);
    CHECK(seat.next()["by"] == "computer");
    CHECK(seat.driver->replaced());
}

TEST_CASE("With no turn timer, an away member is still waited for only as long as the grace period", "[seat][away]")
{
    Seat0 seat;
    seat.driver->away(Clock::now() + milliseconds(30));
    auto play = seat.play();
    const Json::Value prompt = seat.next();
    CHECK(prompt["deadline"].isNull());

    CHECK(play.get() == 1u);
    CHECK(seat.driver->replaced());
}

TEST_CASE("A turn that comes after the grace period is the computer player's, with no prompt", "[seat][away]")
{
    Seat0 seat(milliseconds(60000));
    seat.driver->away(Clock::now() - milliseconds(1));

    CHECK(seat.play().get() == 1u);
    CHECK(seat.recorder.messagesTo(ana).empty());
    CHECK(seat.driver->replaced());
    CHECK(seat.changes == 1);

    // And so is every turn after it.
    CHECK(seat.play().get() == 1u);
    CHECK(seat.recorder.messagesTo(ana).empty());
}

TEST_CASE("Three turns in a row that run out replace a connected member; an answer starts the count over",
          "[seat][timer]")
{
    Seat0 seat(milliseconds(25 * test::slowdown));

    const auto timeOut = [&]
    {
        auto play = seat.play();
        seat.next();
        CHECK(play.get() == 1u);
        CHECK(seat.next()["by"] == "computer");
    };
    const auto answer = [&]
    {
        auto play = seat.play();
        const Json::Value prompt = seat.next();
        seat.driver->submit(seat.answer(PromptKind::Play, prompt["id"].asUInt64(), 1));
        CHECK(play.get() == 1u);
        CHECK(seat.next()["by"] == "you");
    };

    timeOut();
    timeOut();
    answer();
    timeOut();
    timeOut();
    CHECK_FALSE(seat.driver->replaced());
    CHECK(seat.changes == 0);

    timeOut();
    CHECK(seat.driver->replaced());
    CHECK(seat.changes == 1);

    // Replaced, the next turn is made at once and nothing is sent.
    const std::size_t sent = seat.recorder.messagesTo(ana).size();
    CHECK(seat.play().get() == 1u);
    CHECK(seat.recorder.messagesTo(ana).size() == sent);
}

TEST_CASE("Replacing a seat closes its open prompt as the computer's, and it can be taken back", "[seat][away]")
{
    Seat0 seat;
    auto play = seat.play();
    const Json::Value prompt = seat.next();

    seat.driver->replace(false);
    CHECK(play.get() == 1u);
    CHECK(seat.next()["by"] == "computer");
    CHECK(seat.driver->submit(seat.answer(PromptKind::Play, prompt["id"].asUInt64(), 1)) == SeatDriver::Submitted::Stale);
    CHECK_FALSE(seat.driver->humanInCharge());

    CHECK(seat.driver->reclaim());
    CHECK_FALSE(seat.driver->reclaim());
    CHECK(seat.driver->humanInCharge());

    // Their next turn is theirs.
    auto again = seat.play();
    const Json::Value second = seat.next();
    CHECK(second["type"] == "prompt");
    CHECK(seat.driver->submit(seat.answer(PromptKind::Play, second["id"].asUInt64(), 1)) == SeatDriver::Submitted::Taken);
    CHECK(again.get() == 1u);
}

TEST_CASE("A member who leaves is sent nothing more, not even that their prompt closed", "[seat][away]")
{
    Seat0 seat;
    auto play = seat.play();
    seat.next();

    seat.driver->replace(true);
    CHECK(play.get() == 1u);
    CHECK(seat.play().get() == 1u);
    CHECK(seat.recorder.messagesTo(ana).size() == 1);

    // Back, they are sent things again.
    CHECK(seat.driver->reclaim());
    auto asked = seat.play();
    CHECK(seat.next()["type"] == "prompt");
    seat.driver->abandon();
    CHECK_THROWS_AS(asked.get(), GameAbandoned);
}

TEST_CASE("A seat with no computer player to stand in is never replaced", "[seat][away]")
{
    test::Recorder recorder;
    Outbox outbox;
    outbox.attach(ana, recorder.connect(ana));
    SeatDriver driver(Seat{0}, ana, ComputerPlayer{}, std::nullopt, outbox, std::make_shared<std::atomic<std::uint64_t>>(0));

    driver.away(Clock::now() - milliseconds(1));
    driver.replace(false);
    CHECK_FALSE(driver.replaced());
    CHECK(driver.humanInCharge());
}

TEST_CASE("An answer and the end of the grace period at the same moment: exactly one wins", "[seat][away][race]")
{
    std::mt19937 random(5);
    int answers = 0;
    int replaced = 0;

    for(int round = 0 ; round < 300 ; round++)
    {
        Seat0 seat;
        seat.driver->away(Clock::now() + milliseconds(2));
        auto bid = seat.bid();
        const Json::Value prompt = seat.next();

        std::this_thread::sleep_for(std::chrono::microseconds(random() % 4000));
        const SeatDriver::Submitted submitted =
            seat.driver->submit(seat.answer(PromptKind::Bet, prompt["id"].asUInt64(), 3));
        const unsigned int made = bid.get();
        const Json::Value closed = seat.next();
        REQUIRE(closed["type"] == "promptClosed");

        if(submitted == SeatDriver::Submitted::Taken)
        {
            answers++;
            CHECK(closed["by"] == "you");
            CHECK(made == 3);
            CHECK_FALSE(seat.driver->replaced());
        }
        else
        {
            replaced++;
            CHECK(closed["by"] == "computer");
            CHECK(made != 1);
            CHECK(seat.driver->replaced());
        }
        CHECK(seat.recorder.messagesTo(ana).size() == 2);
    }

    CHECK(answers > 0);
    CHECK(replaced > 0);
}
