#ifndef WHIST_CORE_STATUS_H
#define WHIST_CORE_STATUS_H

#include "core/Protocol.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>

namespace whist
{

// What the server is doing, as the log reports it once in a while.
struct ServerStatus
{
    std::size_t connections = 0;
    std::size_t tablesInLobby = 0;
    std::size_t tablesPlaying = 0;
    std::size_t tablesFinished = 0;

    // Game threads running, which is not the same as tables playing: a finished game's table stays.
    std::size_t gameThreads = 0;

    // Of those, the games someone is at, which are the ones max_running_games limits.
    std::size_t attendedGames = 0;

    // Moves a computer player made for a member, since the server started.
    std::uint64_t standInMoves = 0;

    // Processor time the server has used since it started, and how much of one core that was over
    // the time since the last report.
    double cpuSeconds = 0;
    double cpuPercent = 0;

    // The most memory the server has held, in megabytes.
    std::size_t peakMemoryMb = 0;
};

// One line of key=value pairs that starts with "status ", so a log search finds every one.
std::string statusLine(const ServerStatus& status);

// Processor time and peak memory of this process, as the system counts them.
struct ProcessUse
{
    double cpuSeconds = 0;
    std::size_t peakMemoryMb = 0;
};

ProcessUse processUse();

// Reads the process's use now and then, and says how much of a core it used between two reads.
class CpuMeter
{
public:
    // `read` and `now` are the system's unless a test gives its own.
    explicit CpuMeter(std::function<ProcessUse()> read = processUse, std::function<Clock::time_point()> now = Clock::now);

    // Fills in the CPU and memory of `status`: the share of a core is over the time since the last
    // call, or since the meter was made.
    void measure(ServerStatus& status);

private:
    const std::function<ProcessUse()> read_;
    const std::function<Clock::time_point()> now_;
    double lastCpuSeconds_;
    Clock::time_point lastAt_;
};

} // namespace whist

#endif
