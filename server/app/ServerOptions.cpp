#include "app/ServerOptions.h"

#include <json/json.h>

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
            options.address = readString(value, key);
        else if(key == "port")
            options.port = static_cast<std::uint16_t>(readCount(value, key, 0, std::numeric_limits<std::uint16_t>::max()));
        else if(key == "document_root")
            options.documentRoot = readString(value, key);
        else if(key == "io_threads")
            options.ioThreads = readCount(value, key, 1, 64);
        else if(key == "max_message_bytes")
            options.maxMessageBytes = readCount(value, key, 1, 1024 * 1024);
        else if(key == "ping_interval_ms")
            options.pingInterval = std::chrono::milliseconds(readCount(value, key, 1, maxMs));
        else if(key == "shutdown_delay_ms")
            options.shutdownDelay = std::chrono::milliseconds(readCount(value, key, 0, maxMs));
        else if(key == "log_level")
        {
            options.logLevel = readString(value, key);
            if(!logLevels.contains(options.logLevel))
                refuse(key, "must be trace, debug, info, warn or error");
        }
        else
            refuse(key, "is not a known setting");
    }

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
