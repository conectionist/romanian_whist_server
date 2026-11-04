#include "app/Server.h"

#include "app/GameController.h"
#include "core/BuildInfo.h"
#include "core/Service.h"

#include <drogon/drogon.h>

#include <algorithm>
#include <atomic>
#include <csignal>
#include <filesystem>
#include <memory>
#include <set>
#include <string>

#include <unistd.h>

namespace whist
{

namespace
{

// Made by configureApp(), and shut down by stopApp() before Drogon's loops go.
std::shared_ptr<Service>& service()
{
    static std::shared_ptr<Service> instance;
    return instance;
}

// Set from the signal handler, so it must stay lock-free. A timer on the main loop acts on it.
std::atomic<bool> stopRequested{false};
static_assert(std::atomic<bool>::is_always_lock_free);

void onStopSignal(int)
{
    stopRequested.store(true);
}

// Drogon's own handler logs inside the signal handler, which can allocate, and would hang if the
// signal landed on a thread holding the allocator's lock. Ours only sets the flag.
void installStopHandlers()
{
    struct sigaction action{};
    action.sa_handler = onStopSignal;
    sigemptyset(&action.sa_mask);
    sigaction(SIGTERM, &action, nullptr);
    sigaction(SIGINT, &action, nullptr);
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
        loop->runAfter(delay, [] { stopApp(); });
    });
}

void announce()
{
    LOG_INFO << "whist_server " << std::string(serverVersion()) << ", engine " << std::string(engineVersion());

    // Drogon opens one socket per I/O thread on each address, so each is listed once.
    std::set<std::string> addresses;
    for(const trantor::InetAddress& address : drogon::app().getListeners())
        addresses.insert(address.toIpPort());
    for(const std::string& address : addresses)
        LOG_INFO << "listening on " << address;
}

// Each listener's listen() is queued on its own I/O loop when the main loop starts the listeners,
// just after the beginning advices. Queued after it on every I/O loop, `done` runs once all listen.
void whenListening(std::function<void()> done)
{
    drogon::app().getLoop()->queueInLoop([done = std::move(done)]
    {
        const std::size_t loops = drogon::app().getThreadNum();
        auto waiting = std::make_shared<std::atomic<std::size_t>>(loops);
        for(std::size_t i = 0 ; i < loops ; i++)
            drogon::app().getIOLoop(i)->queueInLoop([waiting, done]
            {
                if(--*waiting == 0)
                    drogon::app().getLoop()->queueInLoop(done);
            });
    });
}

// A table's link, /t/K7QM4P, is the page itself, which reads the code from its address.
void serveTableLinks(const std::string& documentRoot)
{
    const std::string index = (std::filesystem::path(documentRoot) / "index.html").string();
    drogon::app().registerHandler(
        "/t/{code}",
        [index](const drogon::HttpRequestPtr&, std::function<void(const drogon::HttpResponsePtr&)>&& respond,
                const std::string& code)
        {
            const bool wellFormed = code.size() == 6 && std::ranges::all_of(code, [](char c)
            {
                return (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
            });
            respond(wellFormed ? drogon::HttpResponse::newFileResponse(index)
                               : drogon::HttpResponse::newNotFoundResponse());
        },
        {drogon::Get});
}

} // namespace

void configureApp(const ServerOptions& options, std::function<void()> onListening)
{
    // Drogon makes this folder, with 256 more inside it, at start, and writes there any request body
    // over its in-memory limit. No route takes a body, so bodies are capped at that limit and
    // nothing is written; the folder is one per user, out of the way.
    constexpr std::size_t maxRequestBody = 16 * 1024;
    const std::filesystem::path uploads = std::filesystem::temp_directory_path()
                                        / ("whist_server-uploads-" + std::to_string(::getuid()));

    ServiceOptions serviceOptions;
    serviceOptions.registry.table.instantFloor = options.instantFloor;
    service() = std::make_shared<Service>(std::move(serviceOptions));

    drogon::app()
        .setLogLevel(toLogLevel(options.logLevel))
        .addListener(options.address, options.port)
        .setDocumentRoot(options.documentRoot)
        .setHomePage("index.html")
        .setUploadPath(uploads.string())
        .setClientMaxMemoryBodySize(maxRequestBody)
        .setClientMaxBodySize(maxRequestBody)
        .setThreadNum(options.ioThreads)
        .setClientMaxWebSocketMessageSize(options.maxMessageBytes)
        .registerController(std::make_shared<GameController>(service(), options.pingInterval))
        .disableSigtermHandling()
        .registerBeginningAdvice([shutdownDelay = options.shutdownDelay, onListening = std::move(onListening)]
        {
            watchForStop(shutdownDelay);
            whenListening([onListening]
            {
                announce();
                if(onListening)
                    onListening();
            });
        });

    serveTableLinks(options.documentRoot);
    installStopHandlers();
}

void stopApp()
{
    if(service())
        service()->shutdown();
    drogon::app().quit();
}

} // namespace whist
