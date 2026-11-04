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

Table::Table(std::string code, Mode mode, Seating seating, TableOptions options, Outbox& outbox,
             std::shared_ptr<Joiner> joiner, std::uint64_t seed)
    : code_(std::move(code)),
      mode_(mode),
      options_(options),
      outbox_(outbox),
      joiner_(std::move(joiner)),
      seating_(std::move(seating)),
      random_(seed)
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
    for(const TableMember& member : seating_.members())
        ids.push_back(member.id);
    return ids;
}

bool Table::hasMember(MemberId member) const
{
    std::lock_guard lock(mutex_);
    return seating_.hasMember(member);
}

Settings Table::settings() const
{
    std::lock_guard lock(mutex_);
    return seating_.settings();
}

std::optional<RunOutcome> Table::outcome() const
{
    std::lock_guard lock(mutex_);
    if(!run_)
        return std::nullopt;
    return run_->outcome();
}

std::size_t Table::scoredRounds() const
{
    std::lock_guard lock(mutex_);
    return run_ ? run_->publisher().history().size() : 0;
}

TableState Table::stateLocked() const
{
    // Nothing on the game thread calls back into the table, so a game's end is read off its run.
    if(state_ == TableState::Playing && run_ && run_->over())
        return TableState::Finished;
    return state_;
}

TableView Table::viewLocked(MemberId member) const
{
    const Settings& settings = seating_.settings();

    TableView view;
    view.code = code_;
    view.mode = mode_;
    view.state = stateLocked();
    view.host = seating_.seatOf(seating_.host());
    view.settings = settings;
    view.paused = paused_ && view.state == TableState::Playing;

    // Only a game under way has seats a computer player took over.
    const bool playing = run_ && view.state == TableState::Playing;
    if(playing && mode_ == Mode::Online)
        view.waitingFor = run_->waitingFor();

    for(unsigned int i = 0 ; i < settings.seats.size() ; i++)
    {
        const SeatSetting& seat = settings.seats[i];
        TableSeatView seatView;
        seatView.kind = seat.kind;
        seatView.level = seat.level;
        seatView.profile = seat.profile;

        if(seat.kind == SeatKind::Human)
        {
            const std::optional<MemberId> holder = seating_.memberAt(i);
            if(playing && run_->replaced(i))
                seatView.presence = Presence::Replaced;
            else if(holder)
                seatView.presence = outbox_.isConnected(*holder) ? Presence::Here : Presence::Away;
            if(holder)
                seatView.you = *holder == member;
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

void Table::broadcastLocked(std::optional<MemberId> except) const
{
    for(const TableMember& member : seating_.members())
    {
        if(member.id != except)
            outbox_.send(member.id, tableStateMessage(viewLocked(member.id)));
    }
}

std::optional<Refusal> Table::startGame(MemberId by)
{
    std::unique_ptr<GameRun> previous;
    {
        std::lock_guard lock(mutex_);
        if(state_ == TableState::Closed)
            return ErrorCode::NotFound;
        if(by != seating_.host())
            return ErrorCode::NotHost;
        if(stateLocked() == TableState::Playing)
            return ErrorCode::Illegal;

        // Seats of those who left the last game are open for this one.
        settleLocked();

        // Not badMessage: the message was sound, and the table is not ready yet.
        if(seating_.hasOpenSeat())
            return Refusal(ErrorCode::Illegal, "seats");
        if(const std::optional<SettingsError> error = checkSettings(seating_.settings(), mode_))
            return Refusal(ErrorCode::Illegal, error->field);

        const Settings& settings = seating_.settings();
        GameRunSetup setup;
        setup.game = game_ + 1;
        setup.settings = settings;
        setup.seatMembers.resize(settings.seats.size());
        for(const TableMember& member : seating_.members())
        {
            if(member.seat)
                setup.seatMembers.at(*member.seat) = member.id;
            else
                setup.watchers.push_back(member.id);
        }
        setup.shuffleSeed = settings.seed;
        setup.instantFloor = options_.instantFloor;
        setup.threadNiceness = options_.threadNiceness;
        setup.promptIds = promptIds_;
        setup.changed = [self = weak_from_this(), game = setup.game]
        {
            if(const std::shared_ptr<Table> table = self.lock())
                table->runChanged(game);
        };
        if(mode_ == Mode::Online)
        {
            setup.roundHold = options_.roundHold;
            if(options_.turnTimerOverride && setup.settings.turnTimer && setup.settings.turnTimer->count() > 0)
                setup.settings.turnTimer = options_.turnTimerOverride;
        }

        auto run = std::make_unique<GameRun>(std::move(setup), outbox_);
        const TableState before = state_;
        const bool pausedBefore = paused_;
        previous = std::exchange(run_, std::move(run));
        game_++;
        state_ = TableState::Playing;
        paused_ = false;

        // Members whose connection is gone are waited for through their grace period at most, counted
        // from when they went.
        if(mode_ == Mode::Online)
        {
            const Clock::time_point now = Clock::now();
            for(const TableMember& member : seating_.members())
            {
                if(outbox_.isConnected(member.id))
                    continue;
                const Clock::time_point since = member.awaySince.value_or(now);
                seating_.setAway(member.id, since);
                run_->away(member.id, since + options_.gracePeriod);
            }
        }
        refreshPresenceLocked();

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

std::optional<Refusal> Table::answer(MemberId member, const Answer& answer)
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

std::optional<Refusal> Table::checkSoloPlayingLocked(MemberId by) const
{
    if(state_ == TableState::Closed)
        return ErrorCode::NotFound;
    if(by != seating_.host())
        return ErrorCode::NotHost;
    if(mode_ != Mode::Solo || stateLocked() != TableState::Playing || !run_)
        return ErrorCode::Illegal;
    return std::nullopt;
}

std::optional<Refusal> Table::setPaused(MemberId by, bool paused)
{
    std::lock_guard lock(mutex_);
    if(std::optional<Refusal> refused = checkSoloPlayingLocked(by))
        return refused;
    if(paused_ == paused)
        return std::nullopt;

    paused_ = paused;
    run_->setPaused(paused);
    broadcastLocked();
    return std::nullopt;
}

std::optional<Refusal> Table::updateSettings(MemberId by, const Settings& settings)
{
    if(mode_ == Mode::Online)
    {
        std::lock_guard lock(mutex_);
        settleLocked();
        if(std::optional<Refusal> refused = checkBetweenGamesLocked(by))
            return refused;
        if(std::optional<Refusal> refused = seating_.updateSettings(by, settings))
            return refused;
        broadcastLocked();
        return std::nullopt;
    }

    if(const std::optional<SettingsError> error = checkSettings(settings, mode_))
        return Refusal(ErrorCode::BadMessage, error->field);

    std::lock_guard lock(mutex_);
    if(state_ == TableState::Closed)
        return ErrorCode::NotFound;
    if(by != seating_.host())
        return ErrorCode::NotHost;

    Settings unchanged = settings;
    unchanged.botSpeed = seating_.settings().botSpeed;
    unchanged.waitAfterRound = seating_.settings().waitAfterRound;
    if(unchanged != seating_.settings())
        return ErrorCode::Illegal;

    seating_.setPacing(settings.botSpeed, settings.waitAfterRound);
    if(run_)
        run_->setPacing(pacingFor(settings.botSpeed, options_.instantFloor), settings.waitAfterRound);
    broadcastLocked();
    return std::nullopt;
}

std::optional<Refusal> Table::continueRound(MemberId by, unsigned int round)
{
    std::lock_guard lock(mutex_);
    if(mode_ == Mode::Online)
    {
        if(state_ == TableState::Closed)
            return ErrorCode::NotFound;
        if(!seating_.hasMember(by))
            return ErrorCode::Illegal;

        // A Continue that arrives just after the game ended is not worth an error.
        if(run_ && stateLocked() == TableState::Playing)
            run_->continueRound(by, round);
        return std::nullopt;
    }

    if(std::optional<Refusal> refused = checkSoloPlayingLocked(by))
    {
        if(refused == ErrorCode::Illegal)
            return std::nullopt;
        return refused;
    }
    run_->continueRound(by, round);
    return std::nullopt;
}

void Table::syncLocked(MemberId member) const
{
    TableView view = viewLocked(member);

    // The game's sync only to someone who sees it from the seat they hold now, so its snapshot's
    // `you` always agrees with the table's.
    const std::optional<Viewer> seen = run_ ? run_->viewer(member) : std::nullopt;
    if(!seen || seen->seat != seating_.seatOf(member))
    {
        SyncView sync;
        sync.table = std::move(view);
        outbox_.send(member, syncMessage(sync, Clock::now()));
        return;
    }
    run_->sendSync(member, std::move(view));
}

bool Table::sync(MemberId member)
{
    std::lock_guard lock(mutex_);
    if(state_ == TableState::Closed || !seating_.hasMember(member))
        return false;

    settleLocked();
    arriveLocked(member);
    syncLocked(member);
    if(mode_ == Mode::Online)
        broadcastLocked(member);
    return true;
}

bool Table::settleLocked()
{
    if(!seating_.hasDeparted() || stateLocked() == TableState::Playing)
        return false;

    seating_.openDeparted();
    return true;
}

void Table::arriveLocked(MemberId member)
{
    seating_.setAway(member, std::nullopt);
    if(run_)
    {
        run_->here(member);
        run_->reclaim(member);
    }
    refreshPresenceLocked();
}

void Table::refreshPresenceLocked()
{
    const bool here = std::ranges::any_of(seating_.members(),
                                          [this](const TableMember& m) { return outbox_.isConnected(m.id); });
    if(here)
        idleSince_.reset();
    else if(!idleSince_)
        idleSince_ = Clock::now();

    if(run_)
        run_->setNobodyHere(!here);
}

void Table::back(MemberId member)
{
    std::lock_guard lock(mutex_);
    if(state_ == TableState::Closed || !seating_.hasMember(member))
        return;

    if(run_ && stateLocked() == TableState::Playing && run_->reclaim(member))
        broadcastLocked();
}

void Table::runChanged(unsigned int game)
{
    std::lock_guard lock(mutex_);
    if(state_ == TableState::Closed || game != game_ || !run_)
        return;

    // The end of a game is its gameOver message, not a tableState: only seats that open are news.
    const bool opened = settleLocked();
    if(opened || !run_->over())
        broadcastLocked();
}

bool Table::reap(Clock::time_point now)
{
    std::lock_guard lock(mutex_);
    if(state_ == TableState::Closed)
        return false;

    const std::chrono::milliseconds idleLimit = mode_ == Mode::Solo ? options_.soloIdle : options_.onlineIdle;
    if(idleSince_ && now - *idleSince_ >= idleLimit)
        return true;
    if(mode_ == Mode::Solo)
        return false;

    bool changed = settleLocked();
    const auto goneFor = [&](const TableMember& member)
    {
        return member.awaySince && now >= *member.awaySince + options_.gracePeriod;
    };

    // A turn that came inside the grace period is replaced by its own game thread; any other seat
    // nothing would notice.
    if(run_ && stateLocked() == TableState::Playing)
    {
        for(const TableMember& member : seating_.members())
        {
            if(member.seat && goneFor(member) && !run_->replaced(*member.seat))
            {
                run_->replace(member.id, false);
                changed = true;
            }
        }
    }

    const MemberId host = seating_.host();
    const auto hostSeat = std::ranges::find_if(seating_.members(), [host](const TableMember& m) { return m.id == host; });
    if(hostSeat != seating_.members().end() && goneFor(*hostSeat))
    {
        if(seating_.passHost([this](MemberId id) { return outbox_.isConnected(id); }))
            changed = true;
    }

    if(changed)
        broadcastLocked();
    return false;
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
