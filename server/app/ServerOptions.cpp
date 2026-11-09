#include "app/ServerOptions.h"

#include <json/json.h>

#include <arpa/inet.h>

#include <fstream>
#include <functional>
#include <limits>
#include <memory>
#include <set>
#include <sstream>
#include <stdexcept>

namespace whist
{

namespace
{

const std::set<std::string, std::less<>> logLevels{"trace", "debug", "info", "warn", "error"};

[[noreturn]] void refuse(const std::string& key, const std::string& why)
{
    throw std::invalid_argument("config: \"" + key + "\" " + why);
}

std::string readString(const Json::Value& value, const std::string& key)
{
    if(!value.isString() || value.asString().empty())
        refuse(key, "must be a non-empty string");

    return value.asString();
}

std::uint64_t readCount(const Json::Value& value, const std::string& key, std::uint64_t min, std::uint64_t max)
{
    if(!value.isUInt64() || value.asUInt64() < min || value.asUInt64() > max)
        refuse(key, "must be a whole number from " + std::to_string(min) + " to " + std::to_string(max));

    return value.asUInt64();
}

// Drogon aborts on a host name, so only an address it can parse gets through.
bool isIpAddress(const std::string& text)
{
    in6_addr parsed{};
    return ::inet_pton(AF_INET, text.c_str(), &parsed) == 1 || ::inet_pton(AF_INET6, text.c_str(), &parsed) == 1;
}

} // namespace

ServerOptions parseOptions(std::string_view json)
{
    Json::CharReaderBuilder builder;
    Json::CharReaderBuilder::strictMode(&builder.settings_);
    const std::unique_ptr<Json::CharReader> reader(builder.newCharReader());

    Json::Value root;
    std::string errors;
    if(!reader->parse(json.data(), json.data() + json.size(), &root, &errors))
        throw std::invalid_argument("config: not valid JSON: " + errors);

    if(!root.isObject())
        throw std::invalid_argument("config: must be a JSON object");

    constexpr std::uint64_t maxMs = 24ULL * 60 * 60 * 1000;

    ServerOptions options;
    for(const std::string& key : root.getMemberNames())
    {
        const Json::Value& value = root[key];

        if(key == "address")
        {
            options.address = readString(value, key);
            if(!isIpAddress(options.address))
                refuse(key, "must be an IP address, such as 127.0.0.1 or ::1");
        }
        else if(key == "port")
            options.port = static_cast<std::uint16_t>(readCount(value, key, 1, std::numeric_limits<std::uint16_t>::max()));
        else if(key == "document_root")
            options.documentRoot = readString(value, key);
        else if(key == "io_threads")
            options.ioThreads = readCount(value, key, 1, 64);
        else if(key == "max_message_bytes")
            options.maxMessageBytes = readCount(value, key, 1, 1024 * 1024);
        else if(key == "ping_interval_ms")
            options.pingInterval = std::chrono::milliseconds(readCount(value, key, 1, maxMs));
        else if(key == "pong_timeout_ms")
            options.pongTimeout = std::chrono::milliseconds(readCount(value, key, 1, maxMs));
        else if(key == "reap_interval_ms")
            options.reapInterval = std::chrono::milliseconds(readCount(value, key, 1, maxMs));
        else if(key == "grace_period_ms")
            options.gracePeriod = std::chrono::milliseconds(readCount(value, key, 1, maxMs));
        else if(key == "online_idle_ms")
            options.onlineIdle = std::chrono::milliseconds(readCount(value, key, 1, maxMs));
        else if(key == "solo_idle_ms")
            options.soloIdle = std::chrono::milliseconds(readCount(value, key, 1, maxMs));
        else if(key == "forget_member_ms")
            options.forgetMember = std::chrono::milliseconds(readCount(value, key, 1, maxMs));
        else if(key == "shutdown_delay_ms")
            options.shutdownDelay = std::chrono::milliseconds(readCount(value, key, 0, maxMs));
        else if(key == "instant_floor_ms")
            options.instantFloor = std::chrono::milliseconds(readCount(value, key, 0, 1000));
        else if(key == "log_level")
        {
            options.logLevel = readString(value, key);
            if(!logLevels.contains(options.logLevel))
                refuse(key, "must be trace, debug, info, warn or error");
        }
        else
            refuse(key, "is not a known setting");
    }

    // A pong cannot come before the ping it answers.
    if(options.pongTimeout <= options.pingInterval)
        refuse("pong_timeout_ms", "must be longer than ping_interval_ms");

    return options;
}

ServerOptions loadOptions(const std::string& path)
{
    std::ifstream file(path, std::ios::binary);
    if(!file)
        throw std::invalid_argument("config: cannot open " + path);

    std::ostringstream text;
    text << file.rdbuf();
    return parseOptions(text.str());
}

} // namespace whist
