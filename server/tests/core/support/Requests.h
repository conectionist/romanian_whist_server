#ifndef WHIST_TESTS_CORE_REQUESTS_H
#define WHIST_TESTS_CORE_REQUESTS_H

#include "core/Codec.h"
#include "core/Service.h"

#include "support/Client.h"
#include "support/ManualClock.h"
#include "support/Online.h"
#include "support/Recorder.h"

#include <catch2/catch_test_macros.hpp>

#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace whist::test
{

// A service whose tables play at once and whose limits and drain read `clock`. A test declares its
// Recorder before the service, because game threads post to it until the service is gone.
inline std::shared_ptr<Service> clockedService(const std::shared_ptr<ManualClock>& clock,
                                               const std::function<void(ServiceOptions&)>& tweak = {})
{
    ServiceOptions options;
    options.registry.table.instantFloor = std::chrono::milliseconds(0);
    options.registry.table.threadNiceness = 0;
    options.limits = clock->limits();
    if(tweak)
        tweak(options);
    return std::make_shared<Service>(std::move(options));
}

inline Json::Value createSolo(const Settings& settings)
{
    Json::Value message = typed("createSolo");
    message["settings"] = encodeSettings(settings);
    return message;
}

inline Json::Value createTable(const std::string& name, const Settings& settings = onlineSettings(3, 2))
{
    Json::Value message = typed("createTable");
    message["settings"] = encodeSettings(settings);
    message["profile"] = profile(name);
    return message;
}

inline Json::Value joinTable(const std::string& code, const std::string& name)
{
    Json::Value message = typed("joinTable");
    message["code"] = code;
    message["profile"] = profile(name);
    return message;
}

// Game threads send as they please, so tests look at the errors: those are sent while a message is
// being handled, before send() returns.
inline std::vector<Json::Value> errors(const Recorder& recorder, MemberId label)
{
    std::vector<Json::Value> found;
    for(const Json::Value& message : recorder.messagesTo(label))
    {
        if(message["type"] == "error")
            found.push_back(message);
    }
    return found;
}

inline Json::Value lastError(const Recorder& recorder, MemberId label)
{
    const std::vector<Json::Value> found = errors(recorder, label);
    return found.empty() ? Json::Value() : found.back();
}

// The messages of one type sent to a label.
inline std::vector<Json::Value> messagesOfType(const Recorder& recorder, MemberId label, const std::string& type)
{
    std::vector<Json::Value> found;
    for(const Json::Value& message : recorder.messagesTo(label))
    {
        if(message["type"] == type)
            found.push_back(message);
    }
    return found;
}

inline bool waitUntil(const std::function<bool()>& done)
{
    const auto deadline = Clock::now() + patience;
    while(!done())
    {
        if(Clock::now() > deadline)
            return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return true;
}

// The code of the online table its maker was just sent a sync for.
inline std::string tableCode(Recorder& recorder, MemberId label)
{
    const auto sync = recorder.waitFor(label, [](const Json::Value& m)
    {
        return m["type"] == "sync" && m["table"].isObject();
    }, patience);
    REQUIRE(sync);
    return (*sync)["table"]["code"].asString();
}

} // namespace whist::test

#endif
