#include "core/Codec.h"

#include "support/Recorder.h"
#include "support/ServerMessages.h"

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <variant>

using namespace whist;

namespace
{

const std::string soloSettings =
    R"({"seats":[{"kind":"human","name":"Ana","gender":"female","avatar":"woman-long-red-hair"},)"
    R"({"kind":"computer","level":"cyborg","name":"T-800","gender":"male","avatar":"cyborg-man-eye"}],)"
    R"("structure":"8-1-8","botSpeed":3,"seed":42})";

template<class Message>
Message decodeAs(const std::string& text)
{
    const Decoded decoded = decodeClientMessage(text);
    INFO(text << " -> " << decoded.detail);
    REQUIRE(decoded.message);
    REQUIRE(std::holds_alternative<Message>(*decoded.message));
    return std::get<Message>(*decoded.message);
}

void refused(const std::string& text, const std::string& detail = {})
{
    const Decoded decoded = decodeClientMessage(text);
    INFO(text);
    CHECK_FALSE(decoded.message);
    CHECK(decoded.error == ErrorCode::BadMessage);
    if(!detail.empty())
        CHECK(decoded.detail == detail);
}

} // namespace

TEST_CASE("Every client message decodes", "[codec]")
{
    const Hello hello = decodeAs<Hello>(R"({"type":"hello","protocol":1,"lang":"ro"})");
    CHECK(hello.protocol == 1);
    CHECK(hello.lang == "ro");
    CHECK_FALSE(hello.token);

    const Hello again = decodeAs<Hello>(R"({"type":"hello","protocol":1,"token":"AAAAAAAAAAAAAAAAAAAA-_","lang":"en"})");
    CHECK(again.token == "AAAAAAAAAAAAAAAAAAAA-_");

    const CreateSolo solo = decodeAs<CreateSolo>(R"({"type":"createSolo","settings":)" + soloSettings + "}");
    REQUIRE(solo.settings.seats.size() == 2);
    CHECK(solo.settings.seats[0].kind == SeatKind::Human);
    CHECK(solo.settings.seats[1].level == Level::Cyborg);
    CHECK(solo.settings.structure == romanian_whist::GameStructure::S_818);
    CHECK(solo.settings.botSpeed == 3);
    CHECK(solo.settings.seed == 42u);
    CHECK(solo.settings.foreheadAndHidden);

    decodeAs<CreateTable>(R"({"type":"createTable","settings":)" + soloSettings +
                          R"(,"profile":{"name":"Ana","gender":"female","avatar":"a"}})");
    CHECK(decodeAs<JoinTable>(R"({"type":"joinTable","code":"K7QM4P","profile":{"name":"Ana","gender":"female","avatar":"a"}})").code == "K7QM4P");
    CHECK(decodeAs<UpdateProfile>(R"({"type":"updateProfile","name":"Ana","gender":"female","avatar":"a"})").profile.name == "Ana");
    CHECK(decodeAs<TakeSeat>(R"({"type":"takeSeat","seat":3})").seat == 3);
    CHECK(decodeAs<SetSeat>(R"({"type":"setSeat","seat":1,"kind":"computer","level":"reckoner-beta"})").level == Level::ReckonerBeta);
    CHECK(decodeAs<SetSeat>(R"({"type":"setSeat","seat":1,"kind":"open"})").kind == SeatKind::Open);
    decodeAs<UpdateSettings>(R"({"type":"updateSettings","settings":)" + soloSettings + "}");
    decodeAs<StartGame>(R"({"type":"startGame"})");

    const Answer bet = decodeAs<Answer>(R"({"type":"bet","prompt":58,"value":0})");
    CHECK(bet.kind == PromptKind::Bet);
    CHECK(bet.prompt == 58);
    CHECK(bet.value == 0);

    const Answer play = decodeAs<Answer>(R"({"type":"play","prompt":57,"index":2})");
    CHECK(play.kind == PromptKind::Play);
    CHECK(play.value == 2);

    CHECK(decodeAs<Continue>(R"({"type":"continue","round":4})").round == 4);
    decodeAs<Pause>(R"({"type":"pause"})");
    decodeAs<Resume>(R"({"type":"resume"})");
    decodeAs<LeaveTable>(R"({"type":"leaveTable"})");
    decodeAs<Back>(R"({"type":"back"})");
}

TEST_CASE("A huge answer stays whole until the seat checks it", "[codec]")
{
    const Answer answer = decodeAs<Answer>(R"({"type":"bet","prompt":1,"value":4294967297})");
    CHECK(answer.value == 4294967297ULL);
}

TEST_CASE("Decoding is strict", "[codec]")
{
    refused("");
    refused("[]");
    refused("{}", "type");
    refused(R"({"type":"dance"})", "type");
    refused(R"({"type":"startGame","extra":1})", "extra");
    refused(R"({"type":"bet","prompt":1,"value":3.0})", "value");
    refused(R"({"type":"bet","prompt":1,"value":-1})", "value");
    refused(R"({"type":"bet","prompt":1,"value":"1"})", "value");
    refused(R"({"type":"bet","prompt":1})", "value");
    refused(R"({"type":"play","prompt":1,"value":0})");
    refused(R"({"type":"startGame"} {"type":"startGame"})");
    refused(R"({"type":"startGame","type":"pause"})");
    refused(R"({"type":"startGame"} // comment)");
    refused(R"({'type':'startGame'})");
    refused(R"({"type":"hello","protocol":1,"lang":"de"})", "lang");
    refused(R"({"type":"hello","protocol":1,"token":"short","lang":"en"})", "token");
    refused(R"({"type":"joinTable","code":"k7qm4p","profile":{"name":"Ana","gender":"female","avatar":"a"}})", "code");
    refused(R"({"type":"takeSeat","seat":6})", "seat");
    refused(R"({"type":"setSeat","seat":1,"kind":"human"})", "kind");
    refused(R"({"type":"createSolo","settings":{"seats":[],"structure":"1-1-1"}})", "structure");
    refused(R"({"type":"createSolo","settings":{"seats":[{"kind":"computer","level":"genius","name":"A","gender":"male","avatar":"a"}],"structure":"1-8-1"}})", "level");
    refused(R"({"type":"createSolo","settings":{"seats":[],"structure":"1-8-1","botSpeed":5}})", "botSpeed");
    refused(R"({"type":"createSolo","settings":{"seats":[],"structure":"1-8-1","seed":4294967296}})", "seed");

    // Deep nesting, invalid UTF-8, and a lone surrogate.
    refused(std::string(100, '[') + std::string(100, ']'));
    refused("{\"type\":\"updateProfile\",\"name\":\"\xC3\x28\",\"gender\":\"male\",\"avatar\":\"a\"}");
    refused(R"({"type":"updateProfile","name":"\ud800","gender":"male","avatar":"a"})");
}

TEST_CASE("Settings come back as they went", "[codec]")
{
    const CreateSolo solo = decodeAs<CreateSolo>(R"({"type":"createSolo","settings":)" + soloSettings + "}");
    const Json::Value encoded = encodeSettings(solo.settings);
    CHECK(toText(encoded) == toText(encodeSettings(decodeSettings(encoded))));
    CHECK(encoded["seed"].asUInt() == 42);
    CHECK_FALSE(encoded.isMember("replacement"));
}

TEST_CASE("A blind prompt has no hand, and a deadline is the time left", "[codec]")
{
    using romanian_whist::Card;
    using romanian_whist::Rank;
    using romanian_whist::Suit;

    Prompt prompt;
    prompt.id = 57;
    prompt.kind = PromptKind::Play;
    prompt.hand = {Card{Rank::Ace, Suit::Spades}, Card{Rank::Ten, Suit::Diamonds}};
    prompt.handSize = 2;
    prompt.legal = {0};

    const auto now = Clock::now();
    prompt.deadline = now + std::chrono::milliseconds(60000);
    Json::Value message = promptMessage(prompt, now);
    message["seq"] = 1;
    CHECK(test::shapeError(message).empty());
    CHECK(message["deadline"] == 60000);
    CHECK(message["hand"][1] == "TD");

    prompt.blind = true;
    prompt.hand.clear();
    prompt.deadline.reset();
    message = promptMessage(prompt, now);
    message["seq"] = 2;
    CHECK(test::shapeError(message).empty());
    CHECK_FALSE(message.isMember("hand"));
    CHECK(message["handSize"].asUInt64() == 2);
    CHECK(message["deadline"].isNull());
}

TEST_CASE("An error's detail names a field and nothing else", "[codec]")
{
    Json::Value message = errorMessage(ErrorCode::BadMessage, "value");
    message["seq"] = 1;
    CHECK(test::shapeError(message).empty());
    CHECK(toText(message) == R"({"code":"badMessage","detail":"value","seq":1,"type":"error"})");
}
