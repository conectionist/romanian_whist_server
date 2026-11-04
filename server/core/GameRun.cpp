#include "core/GameRun.h"

#include "core/ComputerPlayer.h"
#include "core/GameAbandoned.h"
#include "core/Log.h"
#include "core/SeatProvider.h"

#include <stdexcept>
#include <string>
#include <utility>

#ifdef __linux__
#include <sys/resource.h>
#include <unistd.h>
#endif

namespace whist
{

using namespace romanian_whist;

namespace
{

std::vector<std::unique_ptr<SeatDriver>> makeDrivers(const GameRunSetup& setup, Outbox& outbox)
{
    if(setup.seatMembers.size() != setup.settings.seats.size())
        throw std::invalid_argument("a member slot is needed for every seat");

    std::vector<std::unique_ptr<SeatDriver>> drivers;
    for(unsigned int i = 0 ; i < setup.settings.seats.size() ; i++)
    {
        const SeatSetting& seat = setup.settings.seats[i];
        if(seat.kind == SeatKind::Open)
            throw std::invalid_argument("a game needs every seat filled");
        const std::optional<std::uint32_t> seed =
            setup.shuffleSeed ? std::optional<std::uint32_t>(*setup.shuffleSeed + i) : std::nullopt;

        // A human seat has a computer player only when one may stand in for them.
        ComputerPlayer computer;
        if(seat.kind == SeatKind::Computer)
            computer = makeComputerPlayer(*seat.level, Seat{i}, seed);
        else if(setup.settings.replacement)
            computer = makeComputerPlayer(*setup.settings.replacement, Seat{i}, seed);

        const std::optional<MemberId> member = seat.kind == SeatKind::Human ? setup.seatMembers[i] : std::nullopt;
        if(seat.kind == SeatKind::Human && !member)
            throw std::invalid_argument("a human seat needs its member");

        drivers.push_back(std::make_unique<SeatDriver>(Seat{i}, member, std::move(computer),
                                                       setup.settings.turnTimer, outbox, setup.promptIds));
    }
    return drivers;
}

std::vector<Viewer> viewersOf(const GameRunSetup& setup)
{
    std::vector<Viewer> viewers;
    for(unsigned int i = 0 ; i < setup.seatMembers.size() ; i++)
    {
        if(setup.seatMembers[i] && setup.settings.seats[i].kind == SeatKind::Human)
            viewers.push_back({*setup.seatMembers[i], i});
    }
    for(MemberId watcher : setup.watchers)
        viewers.push_back({watcher, std::nullopt});
    return viewers;
}

void lowerPriority(int niceness)
{
#ifdef __linux__
    // Linux applies a niceness to the one thread named here.
    setpriority(PRIO_PROCESS, static_cast<id_t>(gettid()), niceness);
#else
    (void)niceness;
#endif
}

} // namespace

GameRun::GameRun(GameRunSetup setup, Outbox& outbox)
    : game_(setup.game),
      threadNiceness_(setup.threadNiceness),
      firstObservers_(setup.firstObservers),
      viewers_(viewersOf(setup)),
      drivers_(makeDrivers(setup, outbox)),
      publisher_(setup.game, viewers_, pacingFor(setup.settings.botSpeed, setup.instantFloor),
                 setup.settings.waitAfterRound, setup.roundHold, drivers_, outbox)
{
    GameSetup engineSetup;
    for(unsigned int i = 0 ; i < drivers_.size() ; i++)
        engineSetup.seats.push_back({setup.settings.seats[i].profile.name,
                                     std::make_unique<SeatProvider>(*drivers_[i], engine_, Seat{i})});
    engineSetup.structure = setup.settings.structure;
    engineSetup.endWithForeheadAndHidden = setup.settings.foreheadAndHidden;
    engineSetup.all1GamesAreForehead = setup.settings.allOneCardForehead;
    engineSetup.shuffleSeed = setup.shuffleSeed;
    engineSetup_ = std::move(engineSetup);
}

GameRun::~GameRun()
{
    close();
}

void GameRun::start()
{
    thread_ = std::jthread([this, setup = std::move(*engineSetup_)]() mutable { play(std::move(setup)); });
    engineSetup_.reset();
}

void GameRun::play(GameSetup setup)
{
    lowerPriority(threadNiceness_);

    // The outcome is set last on every path, so a game that reads as finished sends nothing more.
    try
    {
        for(IGameObserver* observer : firstObservers_)
            engine_.addObserver(observer);
        for(const auto& driver : drivers_)
        {
            if(driver->observer() != nullptr)
                engine_.addObserver(driver->observer());
        }
        engine_.addObserver(&publisher_);

        engine_.start(std::move(setup));
        engine_.run();
        outcome_ = engine_.getStatus() == GameStatus::Finished ? RunOutcome::Finished : RunOutcome::Stopped;
    }
    catch(const GameAbandoned&)
    {
        outcome_ = RunOutcome::Abandoned;
    }
    catch(const std::exception& failure)
    {
        log(LogLevel::Error, std::string("game ") + std::to_string(game_) + " failed: " + failure.what());
        publisher_.announceFailure();
        outcome_ = RunOutcome::Failed;
    }
    catch(...)
    {
        log(LogLevel::Error, "game " + std::to_string(game_) + " failed");
        publisher_.announceFailure();
        outcome_ = RunOutcome::Failed;
    }
}

void GameRun::close()
{
    // Both halves: a seat parked on its human is woken to throw, and a game that is not parked
    // stops at its next boundary. A rest is cut short so that boundary comes at once.
    for(const auto& driver : drivers_)
        driver->abandon();
    publisher_.cancel();
    engine_.requestStop();
}

void GameRun::silence()
{
    publisher_.silence();
    close();
}

bool GameRun::finished() const
{
    return outcome_ != RunOutcome::Running;
}

bool GameRun::over() const
{
    return finished() || publisher_.gameOverSent();
}

void GameRun::setPaused(bool paused)
{
    publisher_.setPaused(paused);
}

void GameRun::continueRound(MemberId member, unsigned int round)
{
    if(const std::optional<Viewer> seen = viewer(member))
        publisher_.continueRound(round, seen->seat);
}

void GameRun::excuse(MemberId member)
{
    if(const std::optional<Viewer> seen = viewer(member); seen && seen->seat)
        publisher_.excuse(*seen->seat);
}

void GameRun::setPacing(Pacing pacing, bool waitAfterRound)
{
    publisher_.setPacing(pacing, waitAfterRound);
}

RunOutcome GameRun::outcome() const
{
    return outcome_;
}

unsigned int GameRun::game() const
{
    return game_;
}

std::optional<Viewer> GameRun::viewer(MemberId member) const
{
    for(const Viewer& viewer : viewers_)
    {
        if(viewer.member == member)
            return viewer;
    }
    return std::nullopt;
}

SeatDriver::Submitted GameRun::submit(MemberId member, const Answer& answer)
{
    const std::optional<Viewer> seen = viewer(member);
    if(!seen || !seen->seat)
        return SeatDriver::Submitted::NotYourTurn;
    return drivers_.at(*seen->seat)->submit(answer);
}

void GameRun::sendSync(MemberId member, TableView table)
{
    const std::optional<Viewer> seen = viewer(member);
    if(!seen || !seen->seat)
    {
        publisher_.sendSync(member, std::move(table), std::nullopt);
        return;
    }

    drivers_.at(*seen->seat)->withOpenPrompt([&](const std::optional<Prompt>& prompt)
    {
        publisher_.sendSync(member, std::move(table), prompt);
    });
}

const Publisher& GameRun::publisher() const
{
    return publisher_;
}

} // namespace whist
