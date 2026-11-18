#include "support/ScriptedHuman.h"

#include "support/ServerMessages.h"

namespace whist::test
{

ScriptedHuman::ScriptedHuman(Recorder& recorder, MemberId member, Answerer answer, Behaviour behaviour,
                             OnPrompt onPrompt, Continuer onHeld)
    : recorder_(recorder),
      member_(member),
      answer_(std::move(answer)),
      behaviour_(behaviour),
      onPrompt_(std::move(onPrompt)),
      onHeld_(std::move(onHeld)),
      thread_([this] { run(); })
{
}

ScriptedHuman::~ScriptedHuman()
{
    stop_ = true;
    thread_.join();
}

bool ScriptedHuman::waitUntilOver(std::chrono::milliseconds timeout)
{
    std::unique_lock lock(mutex_);
    return changed_.wait_for(lock, timeout, [this] { return over_; });
}

bool ScriptedHuman::waitForPrompt(std::chrono::milliseconds timeout)
{
    std::unique_lock lock(mutex_);
    return changed_.wait_for(lock, timeout, [this] { return counts_.prompts > 0; });
}

ScriptedHuman::Counts ScriptedHuman::counts() const
{
    std::lock_guard lock(mutex_);
    return counts_;
}

std::vector<std::string> ScriptedHuman::problems() const
{
    std::lock_guard lock(mutex_);
    return problems_;
}

void ScriptedHuman::run()
{
    std::size_t cursor = 0;
    while(!stop_)
    {
        const std::optional<Json::Value> message = recorder_.next(member_, cursor, std::chrono::milliseconds(20));
        if(message)
            handle(*message);
    }
}

Answer ScriptedHuman::goodAnswer(const Json::Value& prompt) const
{
    Answer answer;
    answer.prompt = prompt["id"].asUInt64();
    if(prompt["kind"].asString() == "bet")
    {
        answer.kind = PromptKind::Bet;
        const bool zeroForbidden = prompt["forbidden"].isUInt() && prompt["forbidden"].asUInt() == 0;
        answer.value = zeroForbidden ? 1 : 0;
    }
    else
    {
        answer.kind = PromptKind::Play;
        answer.value = prompt["legal"][0].asUInt64();
    }
    return answer;
}

void ScriptedHuman::handle(const Json::Value& message)
{
    const std::string type = message["type"].asString();
    {
        std::lock_guard lock(mutex_);
        counts_.messages++;
        if(const std::string error = shapeError(message); !error.empty())
            problems_.push_back(type + ": " + error);

        if(type == "promptClosed")
        {
            if(message["by"].asString() == "you")
                counts_.closedByYou++;
            else
                counts_.closedByComputer++;
        }
        else if(type == "error")
        {
            const std::string code = message["code"].asString();
            if(code == "illegal")
                counts_.illegal++;
            else if(code == "stalePrompt")
                counts_.stale++;
            else if(code == "notYourTurn")
                counts_.notYourTurn++;
            else
                problems_.push_back("unexpected error " + code);
        }
        else if((type == "game" && (message["event"]["kind"] == "gameOver" || message["event"]["kind"] == "gameStopped")) ||
                (type == "tableState" && message["state"] == "closed"))
        {
            over_ = true;
            changed_.notify_all();
        }
    }

    if(type == "game" || type == "sync")
    {
        std::lock_guard lock(mutex_);
        const Json::Value& snapshot = message["snapshot"];
        if(snapshot.isObject())
            seat_ = snapshot["you"].isUInt() ? std::optional<unsigned int>(snapshot["you"].asUInt()) : std::nullopt;
    }

    if(type == "held")
    {
        // A hold is sent again as others continue; it is continued once, and only when this seat is
        // awaited, or nobody's is, as for a watcher.
        const unsigned int round = message["round"].asUInt();
        bool awaited = message["waitingFor"].empty();
        bool first = false;
        {
            std::lock_guard lock(mutex_);
            for(const Json::Value& seat : message["waitingFor"])
                awaited = awaited || (seat_ && seat.asUInt() == *seat_);
            first = !lastHeld_ || *lastHeld_ != round;
            if(first)
            {
                counts_.holds++;
                lastHeld_ = round;
            }
        }
        if(first && awaited && onHeld_)
            onHeld_(round);
        return;
    }

    if(type != "prompt")
        return;

    std::size_t attempt = 0;
    {
        std::lock_guard lock(mutex_);
        const std::uint64_t id = message["id"].asUInt64();
        if(id != lastPrompt_)
        {
            counts_.prompts++;
            lastPrompt_ = id;
            attempts_ = 0;
        }
        attempt = attempts_++;
        changed_.notify_all();
    }

    if(behaviour_ == Behaviour::Silent)
        return;

    if(attempt == 0 && onPrompt_ && onPrompt_(message))
        return;

    const Answer good = goodAnswer(message);
    if(behaviour_ == Behaviour::Garbage && attempt == 0)
    {
        Answer stale = good;
        stale.prompt += 1000;
        answer_(stale);

        // Wrong kind, an index or bid out of range, or the forbidden bid, by turns.
        Answer illegal = good;
        switch(good.prompt % 3)
        {
            case 0:
                illegal.kind = good.kind == PromptKind::Bet ? PromptKind::Play : PromptKind::Bet;
                break;
            case 1:
                illegal.value = 1'000'000'000'000ULL;
                break;
            default:
                if(good.kind == PromptKind::Bet)
                    illegal.value = message["forbidden"].isUInt() ? message["forbidden"].asUInt() : message["max"].asUInt() + 1;
                else
                    illegal.value = message.isMember("hand") ? message["hand"].size() : message["handSize"].asUInt64();
                break;
        }
        answer_(illegal);
        return;
    }

    answer_(good);
    if(behaviour_ == Behaviour::Garbage)
        answer_(good);
}

} // namespace whist::test
