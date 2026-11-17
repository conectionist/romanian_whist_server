#include "core/Service.h"

#include "core/BuildInfo.h"
#include "core/Codec.h"

#include <utility>

namespace whist
{

Service::Service(ServiceOptions options)
    : version_(options.version.empty() ? std::string(serverVersion()) : std::move(options.version)),
      commit_(options.commit.empty() ? std::string(serverCommit()) : std::move(options.commit)),
      pongTimeout_(options.pongTimeout),
      limits_(std::move(options.limits)),
      registry_(outbox_, std::move(options.registry))
{
}

Service::~Service()
{
    shutdown();
}

Outbox& Service::outbox()
{
    return outbox_;
}

Registry& Service::registry()
{
    return registry_;
}

Limits& Service::limits()
{
    return limits_;
}

const std::string& Service::version() const
{
    return version_;
}

const std::string& Service::commit() const
{
    return commit_;
}

Service::DrainCounts Service::beginDrain(Clock::time_point deadline)
{
    if(registry_.beginDrain())
        outbox_.announce([this, deadline] { return restartingMessage(deadline, limits_.now()); });

    return {registry_.runningGames(), registry_.attendedGames(Clock::now())};
}

bool Service::draining() const
{
    return registry_.draining();
}

ServerStatus Service::status() const
{
    const Registry::TableCounts tables = registry_.tableCounts();

    ServerStatus status;
    status.connections = outbox_.openLinks();
    status.tablesInLobby = tables.lobby;
    status.tablesPlaying = tables.playing;
    status.tablesFinished = tables.finished;
    status.gameThreads = registry_.runningGames();
    status.standInMoves = registry_.standInMoves();
    return status;
}

void Service::shutdown()
{
    registry_.shutdown();
    outbox_.stop();
}

void Service::reap(Clock::time_point now)
{
    for(const MemberId member : outbox_.closeSilent(now - pongTimeout_))
    {
        if(const std::shared_ptr<Table> table = registry_.tableOf(member))
            table->presenceChanged(member);
    }
    registry_.reap(now);
}

} // namespace whist
