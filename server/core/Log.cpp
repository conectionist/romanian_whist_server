#include "core/Log.h"

#include <iostream>
#include <mutex>
#include <string>

namespace whist
{

namespace
{

std::mutex sinkMutex;
LogSink sink;

} // namespace

void setLogSink(LogSink newSink)
{
    std::lock_guard lock(sinkMutex);
    sink = std::move(newSink);
}

void log(LogLevel level, std::string_view line)
{
    std::lock_guard lock(sinkMutex);
    if(sink)
    {
        sink(level, line);
        return;
    }

    const char* prefix = level == LogLevel::Error ? "error: " : level == LogLevel::Warn ? "warn: " : "";
    std::cerr << prefix << line << '\n';
}

} // namespace whist
