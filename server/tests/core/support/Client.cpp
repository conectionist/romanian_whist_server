#include "support/Client.h"

#include "core/Codec.h"

#include <catch2/catch_test_macros.hpp>

namespace whist::test
{

Json::Value typed(std::string_view type)
{
    Json::Value message(Json::objectValue);
    message["type"] = std::string(type);
    return message;
}

Client::Client(std::shared_ptr<Service> service, Recorder& recorder, MemberId label)
    : recorder_(recorder),
      label_(label),
      session_(std::move(service), recorder.connect(label))
{
}

void Client::sendText(std::string_view text)
{
    std::lock_guard lock(mutex_);
    session_.receive(text);
}

void Client::send(const Json::Value& message)
{
    sendText(toText(message));
}

void Client::sendHello(const std::optional<std::string>& token)
{
    Json::Value message = typed("hello");
    message["protocol"] = protocolVersion;
    message["lang"] = "en";
    if(token)
        message["token"] = *token;
    send(message);
}

Json::Value Client::hello(const std::optional<std::string>& token)
{
    const std::size_t before = recorder_.messagesTo(label_).size();
    sendHello(token);

    const auto sync = recorder_.waitFor(label_, [](const Json::Value& m) { return m["type"] == "sync"; },
                                        std::chrono::milliseconds(10000 * slowdown));
    REQUIRE(sync);
    const auto messages = recorder_.messagesTo(label_);
    REQUIRE(messages.size() > before);
    REQUIRE(messages[before]["type"] == "welcome");
    return messages[before];
}

void Client::drop()
{
    std::lock_guard lock(mutex_);
    session_.closed();
}

MemberId Client::label() const
{
    return label_;
}

std::optional<MemberId> Client::member()
{
    std::lock_guard lock(mutex_);
    return session_.member();
}

ScriptedHuman::Answerer Client::answerer()
{
    return [this](const Answer& answer)
    {
        Json::Value message = typed(answer.kind == PromptKind::Bet ? "bet" : "play");
        message["prompt"] = static_cast<Json::UInt64>(answer.prompt);
        message[answer.kind == PromptKind::Bet ? "value" : "index"] = static_cast<Json::UInt64>(answer.value);
        send(message);
    };
}

ScriptedHuman::Continuer Client::continuer()
{
    return [this](unsigned int round)
    {
        Json::Value message = typed("continue");
        message["round"] = round;
        send(message);
    };
}

} // namespace whist::test
