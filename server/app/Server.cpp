#include "app/Server.h"

#include "app/GameController.h"
#include "app/Http.h"
#include "core/BuildInfo.h"
#include "core/Service.h"
#include "core/Status.h"

#include <drogon/drogon.h>

#include <algorithm>
#include <atomic>
#include <csignal>
#include <filesystem>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

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

// How many times a stop has been asked for. Counted in the signal handler, so it must stay
// lock-free. A timer on the main loop acts on it.
std::atomic<int> stopSignals{0};
static_assert(std::atomic<int>::is_always_lock_free);

void onStopSignal(int)
{
    stopSignals.fetch_add(1);
}

// Drogon's own handler logs inside the signal handler, which can allocate, and would hang if the
// signal landed on a thread holding the allocator's lock. Ours only counts.
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

// The first stop signal starts the drain: no new games, everyone told, and the games that are
// running go on until the last that someone is at is over, or `drainTime` has passed. A game nobody
// is at holds, and cannot end, so it is ended with the rest. A second signal ends them at once.
void watchForStop(std::chrono::milliseconds drainTime)
{
    drogon::app().getLoop()->runEvery(std::chrono::milliseconds(100),
        [drainTime, deadline = std::optional<Clock::time_point>{}, stopping = false]() mutable
    {
        const int signals = stopSignals.load();
        if(stopping || signals == 0 || !service())
            return;

        const Clock::time_point now = Clock::now();
        const bool first = !deadline;
        if(first)
            deadline = now + drainTime;

        const auto [running, attended] = service()->beginDrain(*deadline);
        if(first)
            LOG_INFO << "stop asked for: no new games, " << running << " running, " << attended
                     << " attended, may finish within " << std::chrono::duration_cast<std::chrono::seconds>(drainTime).count()
                     << " s";

        if(attended == 0)
        {
            LOG_INFO << "drained: no attended game is running";
            if(running > 0)
                LOG_INFO << "ending " << running << " unattended running game(s) with the rest";
        }
        else if(signals > 1)
            LOG_INFO << "stop asked for again: ending " << running << " running game(s) now";
        else if(now >= *deadline)
            LOG_INFO << "drain time is up: ending " << running << " running game(s), " << attended << " attended";
        else
            return;

        stopping = true;
        LOG_INFO << "shutting down";
        stopApp();
    });
}

// Closes connections that stopped answering pings. Once the service has shut down this does nothing.
void watchForSilence(std::chrono::milliseconds interval)
{
    drogon::app().getLoop()->runEvery(interval, []
    {
        if(service())
            service()->reap(Clock::now());
    });
}

// Logs what the server is doing, so that a quiet day and a busy one can be told apart afterwards.
void watchStatus(std::chrono::milliseconds interval)
{
    drogon::app().getLoop()->runEvery(interval, [meter = CpuMeter()]() mutable
    {
        if(!service())
            return;

        ServerStatus status = service()->status();
        meter.measure(status);
        LOG_INFO << statusLine(status);
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

// What the page and a monitor ask the server about itself.
void serveStatusPages()
{
    drogon::app().registerHandler(
        "/version",
        [](const drogon::HttpRequestPtr&, std::function<void(const drogon::HttpResponsePtr&)>&& respond)
        {
            respond(versionResponse(service()->version(), service()->commit(), protocolVersion));
        },
        {drogon::Get});
    drogon::app().registerHandler(
        "/health",
        [](const drogon::HttpRequestPtr&, std::function<void(const drogon::HttpResponsePtr&)>&& respond)
        {
            respond(healthResponse(service()->draining()));
        },
        {drogon::Get});
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

    // Drogon serves only the file types on its list, from anywhere under the document root. png and
    // ico are for a favicon.
    const std::vector<std::string> fileTypes{"html", "js", "css", "svg", "webp", "png", "ico", "woff2", "txt", "json"};

    ServiceOptions serviceOptions;
    serviceOptions.registry.table.instantFloor = options.instantFloor;
    serviceOptions.pongTimeout = options.pongTimeout;
    serviceOptions.registry.table.gracePeriod = options.gracePeriod;
    serviceOptions.registry.table.onlineIdle = options.onlineIdle;
    serviceOptions.registry.table.soloIdle = options.soloIdle;
    serviceOptions.registry.forgetMember = options.forgetMember;
    serviceOptions.registry.maxRunningGames = options.maxRunningGames;
    serviceOptions.limits.messagesPerSecond = options.messagesPerSecond;
    serviceOptions.limits.messageBurst = options.messageBurst;
    serviceOptions.limits.connectionsPerAddress = options.maxConnectionsPerIp;
    serviceOptions.limits.tablesPerAddressPerHour = options.tablesPerIpPerHour;
    serviceOptions.limits.failedJoinsPerAddress = options.failedJoinsPerIp;
    serviceOptions.limits.failedJoinWindow = options.failedJoinWindow;
    service() = std::make_shared<Service>(std::move(serviceOptions));

    drogon::app()
        .setLogLevel(toLogLevel(options.logLevel))
        .addListener(options.address, options.port)
        .setDocumentRoot(options.documentRoot)
        .setHomePage("index.html")
        .setFileTypes(fileTypes)
        .setUploadPath(uploads.string())
        .setClientMaxMemoryBodySize(maxRequestBody)
        .setClientMaxBodySize(maxRequestBody)
        .setThreadNum(options.ioThreads)
        .setMaxConnectionNum(options.maxConnections)
        .setClientMaxWebSocketMessageSize(options.maxMessageBytes)
        .enableSendfile(false)
        .enableServerHeader(false)
        .setCustom404Page(notFoundPage())
        .registerPreRoutingAdvice([](const drogon::HttpRequestPtr& request, drogon::AdviceCallback&& answer,
                                     drogon::AdviceChainCallback&& proceed)
        {
            if(hasDotSegment(request->path()))
                answer(drogon::HttpResponse::newNotFoundResponse(request));
            else
                proceed();
        })
        .registerPreSendingAdvice(addPageHeaders)
        .registerController(std::make_shared<GameController>(service(), options.pingInterval,
                                                           TrustedProxies(options.trustedProxies)))
        .disableSigtermHandling()
        .registerBeginningAdvice([drainTime = options.drainTime, reapInterval = options.reapInterval,
                                  statusInterval = options.statusLogInterval, onListening = std::move(onListening)]
        {
            watchForStop(drainTime);
            watchForSilence(reapInterval);
            watchStatus(statusInterval);
            whenListening([onListening]
            {
                announce();
                if(onListening)
                    onListening();
            });
        });

    serveStatusPages();
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
