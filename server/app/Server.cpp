#include "app/Server.h"

#include "app/EchoController.h"
#include "core/BuildInfo.h"

#include <drogon/drogon.h>

#include <atomic>
#include <memory>
#include <set>
#include <string>

namespace whist
{

namespace
{

// Drogon calls the SIGTERM and SIGINT handlers inside the signal handler itself, where taking a
// lock can deadlock. So the handler only sets this, and a timer on the main loop acts on it.
std::atomic<bool> stopRequested{false};
static_assert(std::atomic<bool>::is_always_lock_free);

void requestStop()
{
    stopRequested.store(true);
}

trantor::Logger::LogLevel toLogLevel(const std::string& name)
{
    if(name == "trace")
        return trantor::Logger::kTrace;
    if(name == "debug")
        return trantor::Logger::kDebug;
    if(name == "warn")
        return trantor::Logger::kWarn;
    if(name == "error")
        return trantor::Logger::kError;
    return trantor::Logger::kInfo;
}

// The server keeps serving for the delay after a stop is asked for, which is what a drain needs.
void watchForStop(std::chrono::milliseconds delay)
{
    trantor::EventLoop* loop = drogon::app().getLoop();
    loop->runEvery(std::chrono::milliseconds(200), [loop, delay, stopping = false]() mutable
    {
        if(stopping || !stopRequested.load())
            return;

        stopping = true;
        LOG_INFO << "shutting down";
        loop->runAfter(delay, [] { drogon::app().quit(); });
    });
}

} // namespace

void configureApp(const ServerOptions& options)
{
    drogon::app()
        .setLogLevel(toLogLevel(options.logLevel))
        .addListener(options.address, options.port)
        .setDocumentRoot(options.documentRoot)
        .setHomePage("index.html")
        .setThreadNum(options.ioThreads)
        .setClientMaxWebSocketMessageSize(options.maxMessageBytes)
        .registerController(std::make_shared<EchoController>(options.pingInterval))
        .setTermSignalHandler(requestStop)
        .setIntSignalHandler(requestStop)
        .registerBeginningAdvice([shutdownDelay = options.shutdownDelay]
        {
            // Beginning advices run just before the listeners start listening. Queued, this runs
            // just after, so whoever reads "listening on" can connect at once.
            drogon::app().getLoop()->queueInLoop([shutdownDelay]
            {
                LOG_INFO << "whist_server " << std::string(serverVersion()) << ", engine " << std::string(engineVersion());
                // Drogon opens one socket per I/O thread on each address, so each is listed once.
                std::set<std::string> addresses;
                for(const trantor::InetAddress& address : drogon::app().getListeners())
                    addresses.insert(address.toIpPort());
                for(const std::string& address : addresses)
                    LOG_INFO << "listening on " << address;

                watchForStop(shutdownDelay);
            });
        });
}

} // namespace whist
