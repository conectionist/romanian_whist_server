#include "core/Session.h"

#include "core/Codec.h"

#include <type_traits>
#include <utility>
#include <variant>

namespace whist
{

Session::Session(std::shared_ptr<Service> service, std::shared_ptr<Connection> connection)
    : service_(std::move(service)),
      connection_(std::move(connection)),
      link_(service_->outbox().open(connection_))
{
}

std::optional<MemberId> Session::member() const
{
    return member_;
}

void Session::receive(std::string_view text)
{
    if(closing_)
        return;

    Decoded decoded = decodeClientMessage(text);
    if(!decoded.message)
    {
        refuseMalformed(decoded.error, decoded.detail);
        return;
    }

    if(!member_ && !std::holds_alternative<Hello>(*decoded.message))
    {
        refuseMalformed(ErrorCode::BadMessage, "type");
        return;
    }

    // A newer connection has the member, and has told this one to close.
    if(member_ && !current())
        return;

    handle(*decoded.message);
}

void Session::receiveBinary()
{
    if(!closing_)
        refuseMalformed(ErrorCode::BadMessage, {});
}

void Session::heard()
{
    service_->outbox().heard(*link_);
}

void Session::closed()
{
    closing_ = true;
    detach();
}

void Session::detach()
{
    // A connection a newer one has replaced is no longer the member's, and says nothing.
    if(!member_ || !service_->outbox().detach(*member_, connection_.get()))
        return;
    if(const auto table = service_->registry().tableOf(*member_))
        table->presenceChanged(*member_);
}

void Session::handle(const ClientMessage& message)
{
    Registry& registry = service_->registry();
    const MemberId member = member_.value_or(0);
    const auto table = [&] { return registry.tableOf(member); };

    // Whatever else a member sends shows they are here, so it takes back a seat a computer player
    // took over. Leaving does not, and the explicit `back` is just that and nothing more.
    if(member_ && !std::holds_alternative<Hello>(message) && !std::holds_alternative<LeaveTable>(message))
    {
        if(const auto at = table())
            at->back(member);
    }

    std::visit([&]([[maybe_unused]] const auto& m)
    {
        using M = std::decay_t<decltype(m)>;
        if constexpr(std::is_same_v<M, Hello>)
            hello(m);
        else if constexpr(std::is_same_v<M, CreateSolo>)
        {
            const Registry::Created created = registry.createSolo(member, m.settings);
            if(created.error)
                refuse(*created.error, created.detail);
        }
        else if constexpr(std::is_same_v<M, CreateTable>)
        {
            const Registry::Created created = registry.createTable(member, m.settings, m.profile);
            if(created.error)
                refuse(*created.error, created.detail);
        }
        else if constexpr(std::is_same_v<M, JoinTable>)
            refuse(registry.joinTable(member, m.code, m.profile));
        else if constexpr(std::is_same_v<M, UpdateProfile>)
        {
            const auto at = table();
            refuse(at ? at->updateProfile(member, m.profile) : Refusal(ErrorCode::Illegal));
        }
        else if constexpr(std::is_same_v<M, TakeSeat>)
        {
            const auto at = table();
            refuse(at ? at->takeSeat(member, m.seat) : Refusal(ErrorCode::Illegal));
        }
        else if constexpr(std::is_same_v<M, SetSeat>)
        {
            const auto at = table();
            refuse(at ? at->setSeat(member, m.seat, m.kind, m.level, lang_) : Refusal(ErrorCode::Illegal));
        }
        else if constexpr(std::is_same_v<M, Answer>)
        {
            const auto at = table();
            refuse(at ? at->answer(member, m) : Refusal(ErrorCode::NotYourTurn));
        }
        else if constexpr(std::is_same_v<M, Pause> || std::is_same_v<M, Resume>)
        {
            const auto at = table();
            refuse(at ? at->setPaused(member, std::is_same_v<M, Pause>) : Refusal(ErrorCode::Illegal));
        }
        else if constexpr(std::is_same_v<M, UpdateSettings>)
        {
            const auto at = table();
            refuse(at ? at->updateSettings(member, m.settings) : Refusal(ErrorCode::Illegal));
        }
        else if constexpr(std::is_same_v<M, Continue>)
        {
            const auto at = table();
            refuse(at ? at->continueRound(member, m.round) : Refusal(ErrorCode::Illegal));
        }
        else if constexpr(std::is_same_v<M, StartGame>)
        {
            const auto at = table();
            refuse(at ? at->startGame(member) : Refusal(ErrorCode::Illegal));
        }
        else if constexpr(std::is_same_v<M, LeaveTable>)
            leave();
    }, message);
}

void Session::hello(const Hello& hello)
{
    if(member_)
    {
        refuseMalformed(ErrorCode::BadMessage, "type");
        return;
    }
    if(hello.protocol != protocolVersion)
    {
        refuse(ErrorCode::Version);
        closeConnection();
        return;
    }

    Registry& registry = service_->registry();
    std::optional<MemberId> id;
    std::string token;
    if(hello.token)
    {
        id = registry.findMember(*hello.token);
        if(id)
            token = *hello.token;
    }

    // No token, or one from before a restart: a new member, whose page keeps the new token.
    if(!id)
    {
        Registry::NewMember created = registry.newMember();
        id = created.id;
        token = std::move(created.token);
    }

    if(!service_->outbox().attach(*id, link_, welcomeMessage(token, *id, service_->version(), service_->commit())))
    {
        closeConnection();
        return;
    }
    member_ = id;
    lang_ = hello.lang;

    // Always a sync, so the page knows to wait for it, and drops whatever came before it.
    const auto table = registry.tableOf(*id);
    if(!table || !table->sync(*id))
        reply(syncMessage(SyncView{}, Clock::now()));
}

void Session::leave()
{
    Registry& registry = service_->registry();
    const auto table = registry.tableOf(*member_);
    if(!table)
    {
        refuse(ErrorCode::Illegal);
        return;
    }

    const Mode mode = table->mode();
    if(const std::optional<Refusal> refused = registry.leaveTable(*member_))
    {
        refuse(*refused);
        return;
    }

    // A closed solo table says so itself. Someone who leaves an online one is told they are at none.
    if(mode == Mode::Online)
        reply(syncMessage(SyncView{}, Clock::now()));
}

void Session::reply(Json::Value message)
{
    service_->outbox().post(*link_, std::move(message));
}

void Session::refuse(ErrorCode code, std::string_view detail)
{
    reply(errorMessage(code, detail));
}

void Session::refuse(const Refusal& refusal)
{
    refuse(refusal.code, refusal.detail);
}

void Session::refuse(const std::optional<Refusal>& refusal)
{
    if(refusal)
        refuse(*refusal);
}

void Session::refuseMalformed(ErrorCode code, std::string_view detail)
{
    refuse(code, detail);
    if(++badMessages_ >= badMessageLimit)
        closeConnection();
}

void Session::closeConnection()
{
    closing_ = true;
    service_->outbox().close(*link_);
    detach();
}

bool Session::current() const
{
    return member_ && service_->outbox().isCurrent(*member_, connection_.get());
}

} // namespace whist
