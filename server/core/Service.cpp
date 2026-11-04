#include "core/Service.h"

#include "core/BuildInfo.h"

#include <utility>

namespace whist
{

Service::Service(ServiceOptions options)
    : version_(options.version.empty() ? std::string(serverVersion()) : std::move(options.version)),
      commit_(options.commit.empty() ? std::string(serverCommit()) : std::move(options.commit)),
      pongTimeout_(options.pongTimeout),
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

const std::string& Service::version() const
{
    return version_;
}

const std::string& Service::commit() const
{
    return commit_;
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
}

} // namespace whist
