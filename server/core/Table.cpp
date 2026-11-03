#include "core/Table.h"

#include "core/Codec.h"

#include <algorithm>
#include <utility>

namespace whist
{

Table::Table(std::string code, Mode mode, Settings settings, std::vector<TableMember> members, MemberId host,
             TableOptions options, Outbox& outbox, std::shared_ptr<Joiner> joiner)
    : code_(std::move(code)),
      mode_(mode),
      options_(options),
      outbox_(outbox),
      joiner_(std::move(joiner)),
      settings_(std::move(settings)),
      members_(std::move(members)),
      host_(host)
{
}

Table::~Table()
{
    if(run_)
        joiner_->hand(std::move(run_));
}

const std::string& Table::code() const
{
    return code_;
}

Mode Table::mode() const
{
    return mode_;
}

TableState Table::state() const
{
    std::lock_guard lock(mutex_);
    return stateLocked();
}

unsigned int Table::game() const
{
    std::lock_guard lock(mutex_);
    return game_;
}

std::vector<MemberId> Table::members() const
{
    std::lock_guard lock(mutex_);
    std::vector<MemberId> ids;
    for(const TableMember& member : members_)
        ids.push_back(member.id);
    return ids;
}

bool Table::hasMember(MemberId member) const
{
    std::lock_guard lock(mutex_);
    return std::ranges::any_of(members_, [member](const TableMember& m) { return m.id == member; });
}

std::optional<RunOutcome> Table::outcome() const
{
    std::lock_guard lock(mutex_);
    if(!run_)
        return std::nullopt;
    return run_->outcome();
}

TableState Table::stateLocked() const
{
    // Nothing on the game thread calls back into the table, so a game's end is read off its run.
    if(state_ == TableState::Playing && run_ && run_->finished())
        return TableState::Finished;
    return state_;
}

std::optional<unsigned int> Table::seatOfLocked(MemberId member) const
{
    for(const TableMember& m : members_)
    {
        if(m.id == member)
            return m.seat;
    }
    return std::nullopt;
}

TableView Table::viewLocked(MemberId member) const
{
    TableView view;
    view.code = code_;
    view.mode = mode_;
    view.state = stateLocked();
    view.host = seatOfLocked(host_);
    view.settings = settings_;

    for(unsigned int i = 0 ; i < settings_.seats.size() ; i++)
    {
        const SeatSetting& seat = settings_.seats[i];
        TableSeatView seatView;
        seatView.kind = seat.kind;
        seatView.level = seat.level;
        seatView.profile = seat.profile;

        if(seat.kind == SeatKind::Human)
        {
            const auto holder = std::ranges::find_if(members_, [i](const TableMember& m) { return m.seat == i; });
            if(holder != members_.end())
            {
                seatView.presence = outbox_.isConnected(holder->id) ? Presence::Here : Presence::Away;
                seatView.you = holder->id == member;
            }
        }
        view.seats.push_back(std::move(seatView));
    }
    return view;
}

TableView Table::viewFor(MemberId member) const
{
    std::lock_guard lock(mutex_);
    return viewLocked(member);
}

void Table::broadcastLocked() const
{
    for(const TableMember& member : members_)
        outbox_.send(member.id, tableStateMessage(viewLocked(member.id)));
}

std::optional<ErrorCode> Table::startGame(MemberId by)
{
    std::unique_ptr<GameRun> previous;
    {
        std::lock_guard lock(mutex_);
        if(state_ == TableState::Closed)
            return ErrorCode::NotFound;
        if(by != host_)
            return ErrorCode::NotHost;
        if(stateLocked() == TableState::Playing)
            return ErrorCode::Illegal;

        GameRunSetup setup;
        setup.game = game_ + 1;
        setup.settings = settings_;
        setup.seatMembers.resize(settings_.seats.size());
        for(const TableMember& member : members_)
        {
            if(member.seat)
                setup.seatMembers.at(*member.seat) = member.id;
            else
                setup.watchers.push_back(member.id);
        }
        setup.shuffleSeed = settings_.seed;
        setup.instantFloor = options_.instantFloor;
        setup.threadNiceness = options_.threadNiceness;
        setup.promptIds = promptIds_;

        auto run = std::make_unique<GameRun>(std::move(setup), outbox_);
        previous = std::exchange(run_, std::move(run));
        game_++;
        state_ = TableState::Playing;

        // The tableState goes out before the game's first message.
        broadcastLocked();
        run_->start();
    }

    joiner_->hand(std::move(previous));
    return std::nullopt;
}

void Table::answer(MemberId member, const Answer& answer)
{
    SeatDriver::Submitted submitted = SeatDriver::Submitted::NotYourTurn;
    {
        std::lock_guard lock(mutex_);
        const std::optional<unsigned int> seat = seatOfLocked(member);
        if(run_ && seat && state_ == TableState::Playing)
            submitted = run_->submit(*seat, answer);
    }

    if(submitted == SeatDriver::Submitted::Stale)
        outbox_.send(member, errorMessage(ErrorCode::StalePrompt));
    else if(submitted == SeatDriver::Submitted::NotYourTurn)
        outbox_.send(member, errorMessage(ErrorCode::NotYourTurn));
}

void Table::sync(MemberId member)
{
    std::lock_guard lock(mutex_);
    if(!std::ranges::any_of(members_, [member](const TableMember& m) { return m.id == member; }))
        return;

    TableView view = viewLocked(member);
    if(!run_)
    {
        SyncView sync;
        sync.table = std::move(view);
        outbox_.send(member, syncMessage(sync, Clock::now()));
        return;
    }
    run_->sendSync(member, seatOfLocked(member), std::move(view));
}

void Table::close()
{
    std::unique_ptr<GameRun> run;
    {
        std::lock_guard lock(mutex_);
        if(state_ == TableState::Closed)
            return;
        state_ = TableState::Closed;
        run = std::move(run_);
        broadcastLocked();
    }
    joiner_->hand(std::move(run));
}

} // namespace whist
