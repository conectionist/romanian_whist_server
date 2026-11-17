#include "core/Status.h"

#include <cstdio>
#include <string>
#include <utility>

#include <sys/resource.h>

namespace whist
{

namespace
{

double seconds(const timeval& time)
{
    return static_cast<double>(time.tv_sec) + static_cast<double>(time.tv_usec) / 1e6;
}

} // namespace

std::string statusLine(const ServerStatus& status)
{
    char cpu[64];
    std::snprintf(cpu, sizeof cpu, "cpu_percent=%.1f cpu_seconds=%.1f", status.cpuPercent, status.cpuSeconds);

    return "status connections=" + std::to_string(status.connections)
         + " tables_lobby=" + std::to_string(status.tablesInLobby)
         + " tables_playing=" + std::to_string(status.tablesPlaying)
         + " tables_finished=" + std::to_string(status.tablesFinished)
         + " game_threads=" + std::to_string(status.gameThreads)
         + " stand_in_moves=" + std::to_string(status.standInMoves)
         + " " + cpu
         + " peak_memory_mb=" + std::to_string(status.peakMemoryMb);
}

ProcessUse processUse()
{
    rusage use{};
    ::getrusage(RUSAGE_SELF, &use);

    // Linux counts ru_maxrss in kilobytes.
    return {seconds(use.ru_utime) + seconds(use.ru_stime), static_cast<std::size_t>(use.ru_maxrss) / 1024};
}

CpuMeter::CpuMeter(std::function<ProcessUse()> read, std::function<Clock::time_point()> now)
    : read_(std::move(read)), now_(std::move(now)), lastCpuSeconds_(read_().cpuSeconds), lastAt_(now_())
{
}

void CpuMeter::measure(ServerStatus& status)
{
    const ProcessUse use = read_();
    const Clock::time_point at = now_();

    const double elapsed = std::chrono::duration<double>(at - lastAt_).count();
    status.cpuSeconds = use.cpuSeconds;
    status.cpuPercent = elapsed > 0 ? 100.0 * (use.cpuSeconds - lastCpuSeconds_) / elapsed : 0.0;
    status.peakMemoryMb = use.peakMemoryMb;

    lastCpuSeconds_ = use.cpuSeconds;
    lastAt_ = at;
}

} // namespace whist
