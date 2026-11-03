#include "core/Outbox.h"

#include "core/Codec.h"

#include <utility>

namespace whist
{

void Outbox::attach(MemberId member, std::shared_ptr<Connection> connection)
{
    auto link = std::make_shared<Link>();
    link->connection = std::move(connection);

    std::lock_guard lock(mutex_);
    links_[member] = std::move(link);
}

void Outbox::detach(MemberId member, const Connection* connection)
{
    std::lock_guard lock(mutex_);
    const auto found = links_.find(member);
    if(found != links_.end() && found->second->connection.get() == connection)
        links_.erase(found);
}

bool Outbox::isConnected(MemberId member) const
{
    std::lock_guard lock(mutex_);
    return links_.contains(member);
}

void Outbox::send(MemberId member, Json::Value message)
{
    std::shared_ptr<Link> link;
    {
        std::lock_guard lock(mutex_);
        const auto found = links_.find(member);
        if(found == links_.end())
            return;
        link = found->second;
    }

    std::lock_guard lock(link->mutex);
    message["seq"] = static_cast<Json::UInt64>(++link->seq);
    link->connection->post(toText(message));
}

} // namespace whist
