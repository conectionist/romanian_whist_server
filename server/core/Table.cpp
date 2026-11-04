#include "core/Table.h"

#include "core/Codec.h"
#include "core/Log.h"
#include "core/Pacing.h"

#include <algorithm>
#include <string>
#include <system_error>
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
    if(state_ == TableState::Playing && run_ && run_->over())
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
    view.paused = paused_ && view.state == TableState::Playing;

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
        if(checkSettings(settings_, mode_))
            return ErrorCode::BadMessage;

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
        const TableState before = state_;
        const bool pausedBefore = paused_;
        previous = std::exchange(run_, std::move(run));
        game_++;
        state_ = TableState::Playing;
        paused_ = false;

        // The tableState goes out before the game's first message.
        broadcastLocked();
        try
        {
            run_->start();
        }
        catch(const std::system_error& failure)
        {
            // No thread was made, so the new run has nothing to join and can go here.
            log(LogLevel::Error, std::string("a game thread could not start: ") + failure.what());
            run_ = std::move(previous);
            game_--;
            state_ = before;
            paused_ = pausedBefore;
            broadcastLocked();
            return ErrorCode::Draining;
        }

        // It has sent gameOver; whatever it might still send must not follow the new game's start.
        if(previous)
            previous->silence();
    }

    joiner_->hand(std::move(previous));
    return std::nullopt;
}

std::optional<ErrorCode> Table::answer(MemberId member, const Answer& answer)
{
    SeatDriver::Submitted submitted = SeatDriver::Submitted::NotYourTurn;
    {
        std::lock_guard lock(mutex_);
        if(run_ && state_ == TableState::Playing)
            submitted = run_->submit(member, answer);
    }

    switch(submitted)
    {
        case SeatDriver::Submitted::Taken: return std::nullopt;
        case SeatDriver::Submitted::Stale: return ErrorCode::StalePrompt;
        case SeatDriver::Submitted::NotYourTurn: return ErrorCode::NotYourTurn;
    }
    return ErrorCode::NotYourTurn;
}

std::optional<ErrorCode> Table::checkSoloPlayingLocked(MemberId by) const
{
    if(state_ == TableState::Closed)
        return ErrorCode::NotFound;
    if(by != host_)
        return ErrorCode::NotHost;
    if(mode_ != Mode::Solo || stateLocked() != TableState::Playing || !run_)
        return ErrorCode::Illegal;
    return std::nullopt;
}

std::optional<ErrorCode> Table::setPaused(MemberId by, bool paused)
{
    std::lock_guard lock(mutex_);
    if(const std::optional<ErrorCode> refused = checkSoloPlayingLocked(by))
        return refused;
    if(paused_ == paused)
        return std::nullopt;

    paused_ = paused;
    run_->setPaused(paused);
    broadcastLocked();
    return std::nullopt;
}

std::optional<ErrorCode> Table::updateSettings(MemberId by, const Settings& settings)
{
    if(checkSettings(settings, mode_))
        return ErrorCode::BadMessage;

    std::lock_guard lock(mutex_);
    if(state_ == TableState::Closed)
        return ErrorCode::NotFound;
    if(by != host_)
        return ErrorCode::NotHost;
    if(mode_ != Mode::Solo)
        return ErrorCode::Illegal;

    Settings unchanged = settings;
    unchanged.botSpeed = settings_.botSpeed;
    unchanged.waitAfterRound = settings_.waitAfterRound;
    if(unchanged != settings_)
        return ErrorCode::Illegal;

    settings_.botSpeed = settings.botSpeed;
    settings_.waitAfterRound = settings.waitAfterRound;
    if(run_)
        run_->setPacing(pacingFor(settings_.botSpeed, options_.instantFloor), settings_.waitAfterRound);
    broadcastLocked();
    return std::nullopt;
}

std::optional<ErrorCode> Table::continueRound(MemberId by, unsigned int round)
{
    std::lock_guard lock(mutex_);
    if(const std::optional<ErrorCode> refused = checkSoloPlayingLocked(by))
    {
        // A Continue that arrives just after the game ended is not worth an error.
        if(refused == ErrorCode::Illegal)
            return std::nullopt;
        return refused;
    }
    run_->continueRound(round);
    return std::nullopt;
}

bool Table::sync(MemberId member)
{
    std::lock_guard lock(mutex_);
    if(state_ == TableState::Closed ||
       !std::ranges::any_of(members_, [member](const TableMember& m) { return m.id == member; }))
        return false;

    TableView view = viewLocked(member);

    // The game's sync only to someone who sees it from the seat they hold now, so its snapshot's
    // `you` always agrees with the table's.
    const std::optional<Viewer> seen = run_ ? run_->viewer(member) : std::nullopt;
    if(!seen || seen->seat != seatOfLocked(member))
    {
        SyncView sync;
        sync.table = std::move(view);
        outbox_.send(member, syncMessage(sync, Clock::now()));
        return true;
    }
    run_->sendSync(member, std::move(view));
    return true;
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
        if(run)
            run->silence();
        broadcastLocked();
    }
    joiner_->hand(std::move(run));
}

} // namespace whist
