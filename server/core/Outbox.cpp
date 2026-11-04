#include "core/Outbox.h"

#include "core/Codec.h"

#include <utility>

namespace whist
{

Outbox::Link::Link(std::shared_ptr<Connection> connection)
    : connection_(std::move(connection))
{
}

const Connection* Outbox::Link::connection() const
{
    return connection_.get();
}

std::shared_ptr<Outbox::Link> Outbox::open(std::shared_ptr<Connection> connection)
{
    return std::make_shared<Link>(std::move(connection));
}

bool Outbox::attach(MemberId member, const std::shared_ptr<Link>& link, Json::Value first)
{
    std::shared_ptr<Link> previous;
    {
        std::shared_lock gate(gate_);
        if(stopped_)
            return false;

        // The member's messages find the link only once `first` is on it, and they wait for its lock.
        std::lock_guard linkLock(link->mutex_);
        if(link->closed_)
            return false;
        if(!first.isNull())
            postLocked(*link, std::move(first));

        std::lock_guard lock(mutex_);
        previous = std::exchange(links_[member], link);
    }

    if(previous && previous != link)
    {
        std::shared_lock gate(gate_);
        if(stopped_)
            return true;
        std::lock_guard linkLock(previous->mutex_);
        postLocked(*previous, errorMessage(ErrorCode::OpenElsewhere));
        closeLocked(*previous);
    }
    return true;
}

void Outbox::attach(MemberId member, std::shared_ptr<Connection> connection)
{
    attach(member, open(std::move(connection)), Json::Value(Json::nullValue));
}

bool Outbox::detach(MemberId member, const Connection* connection)
{
    std::lock_guard lock(mutex_);
    const auto found = links_.find(member);
    if(found == links_.end() || found->second->connection() != connection)
        return false;
    links_.erase(found);
    return true;
}

bool Outbox::isConnected(MemberId member) const
{
    std::lock_guard lock(mutex_);
    return links_.contains(member);
}

bool Outbox::isCurrent(MemberId member, const Connection* connection) const
{
    std::lock_guard lock(mutex_);
    const auto found = links_.find(member);
    return found != links_.end() && found->second->connection() == connection;
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
    post(*link, std::move(message));
}

void Outbox::post(Link& link, Json::Value message)
{
    std::shared_lock gate(gate_);
    if(stopped_)
        return;
    std::lock_guard lock(link.mutex_);
    postLocked(link, std::move(message));
}

void Outbox::close(Link& link)
{
    std::shared_lock gate(gate_);
    if(stopped_)
        return;
    std::lock_guard lock(link.mutex_);
    closeLocked(link);
}

void Outbox::postLocked(Link& link, Json::Value message)
{
    if(link.closed_)
        return;
    message["seq"] = static_cast<Json::UInt64>(++link.seq_);
    link.connection_->post(toText(message));
}

void Outbox::closeLocked(Link& link)
{
    if(link.closed_)
        return;
    link.closed_ = true;
    link.connection_->close();
}

void Outbox::stop()
{
    {
        std::unique_lock gate(gate_);
        stopped_ = true;
    }
    std::lock_guard lock(mutex_);
    links_.clear();
}

} // namespace whist
