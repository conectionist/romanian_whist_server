#include "support/Recorder.h"

#include <catch2/catch_test_macros.hpp>

#include <json/reader.h>

#include <memory>
#include <sstream>
#include <utility>

namespace whist::test
{

namespace
{

class RecordingConnection : public Connection
{
public:
    RecordingConnection(MemberId member, std::function<void(MemberId, std::string)> record)
        : record_(std::move(record)), member_(member)
    {
    }

    void post(std::string text) override
    {
        record_(member_, std::move(text));
    }

private:
    std::function<void(MemberId, std::string)> record_;
    MemberId member_;
};

// Off the test's own thread, where Catch2's assertions must not be used: a message that does not
// parse is recorded as null, which every check then refuses.
Json::Value parseOrNull(const std::string& text)
{
    Json::CharReaderBuilder builder;
    Json::CharReaderBuilder::strictMode(&builder.settings_);
    std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
    Json::Value value;
    std::string errors;
    if(!reader->parse(text.data(), text.data() + text.size(), &value, &errors))
        return Json::Value(Json::nullValue);
    return value;
}

} // namespace

Json::Value parse(const std::string& text)
{
    Json::CharReaderBuilder builder;
    Json::CharReaderBuilder::strictMode(&builder.settings_);
    std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
    Json::Value value;
    std::string errors;
    const bool parsed = reader->parse(text.data(), text.data() + text.size(), &value, &errors);
    INFO(text << "\n" << errors);
    REQUIRE(parsed);
    return value;
}

std::shared_ptr<Connection> Recorder::connect(MemberId member)
{
    return std::make_shared<RecordingConnection>(member, [this](MemberId to, std::string text) { record(to, std::move(text)); });
}

void Recorder::record(MemberId member, std::string text)
{
    Entry entry;
    entry.member = member;
    entry.message = parseOrNull(text);
    entry.text = std::move(text);
    {
        std::lock_guard lock(mutex_);
        byMember_[member].push_back(entries_.size());
        entries_.push_back(std::move(entry));
    }
    changed_.notify_all();
}

void Recorder::mark(Mark mark)
{
    std::lock_guard lock(mutex_);
    Entry entry;
    entry.mark = std::move(mark);
    entries_.push_back(std::move(entry));
}

std::vector<Entry> Recorder::entries() const
{
    std::lock_guard lock(mutex_);
    return entries_;
}

std::vector<Json::Value> Recorder::messagesTo(MemberId member) const
{
    std::lock_guard lock(mutex_);
    std::vector<Json::Value> messages;
    const auto found = byMember_.find(member);
    if(found != byMember_.end())
    {
        for(std::size_t index : found->second)
            messages.push_back(entries_[index].message);
    }
    return messages;
}

std::optional<Json::Value> Recorder::next(MemberId member, std::size_t& cursor, std::chrono::milliseconds timeout)
{
    std::unique_lock lock(mutex_);
    const bool arrived = changed_.wait_for(lock, timeout, [&] { return byMember_[member].size() > cursor; });
    if(!arrived)
        return std::nullopt;
    return entries_[byMember_[member][cursor++]].message;
}

std::optional<Json::Value> Recorder::waitFor(MemberId member, const std::function<bool(const Json::Value&)>& match,
                                             std::chrono::milliseconds timeout)
{
    std::size_t cursor = 0;
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while(true)
    {
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
        if(left.count() <= 0)
            return std::nullopt;
        std::optional<Json::Value> message = next(member, cursor, left);
        if(!message)
            return std::nullopt;
        if(match(*message))
            return message;
    }
}

} // namespace whist::test
