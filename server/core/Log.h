#ifndef WHIST_CORE_LOG_H
#define WHIST_CORE_LOG_H

#include <functional>
#include <string_view>

namespace whist
{

enum class LogLevel
{
    Info,
    Warn,
    Error
};

using LogSink = std::function<void(LogLevel, std::string_view)>;

// The core has no logger of its own. Until a sink is set, lines go to stderr. A sink must not log
// through log() itself, which would deadlock.
void setLogSink(LogSink sink);
void log(LogLevel level, std::string_view line);

} // namespace whist

#endif
