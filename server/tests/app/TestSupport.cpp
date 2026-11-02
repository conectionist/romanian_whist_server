#include "TestSupport.h"

#include "SpikeController.h"
#include "app/Server.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/reporters/catch_reporter_event_listener.hpp>
#include <catch2/reporters/catch_reporter_registrars.hpp>

#include <drogon/drogon.h>

#include <fstream>
#include <future>
#include <memory>
#include <random>
#include <thread>
#include <unistd.h>

namespace whist::test
{

using namespace std::chrono_literals;

TempDir::TempDir()
{
    std::random_device random;
    path_ = std::filesystem::temp_directory_path()
          / ("whist-test-" + std::to_string(::getpid()) + "-" + std::to_string(random()));
    std::filesystem::create_directories(path_);
}

TempDir::~TempDir()
{
    std::error_code ignored;
    std::filesystem::remove_all(path_, ignored);
}

void TempDir::write(const std::string& name, const std::string& contents) const
{
    std::ofstream(path_ / name, std::ios::binary) << contents;
}

namespace
{

struct RunningServer
{
    TempDir documentRoot;
    TestServer info;
    std::thread thread;
};

std::unique_ptr<RunningServer>& runningServer()
{
    static std::unique_ptr<RunningServer> server;
    return server;
}

class StopTestServer : public Catch::EventListenerBase
{
public:
    using EventListenerBase::EventListenerBase;

    void testRunEnded(const Catch::TestRunStats&) override
    {
        std::unique_ptr<RunningServer>& server = runningServer();
        if(!server)
            return;

        spike().joinSenders();
        drogon::app().quit();
        server->thread.join();
        server.reset();
    }
};

} // namespace

CATCH_REGISTER_LISTENER(StopTestServer)

namespace
{

const std::shared_ptr<SpikeController>& spikeController()
{
    static const std::shared_ptr<SpikeController> controller = std::make_shared<SpikeController>();
    return controller;
}

} // namespace

SpikeController& spike()
{
    return *spikeController();
}

const TestServer& testServer()
{
    std::unique_ptr<RunningServer>& server = runningServer();
    if(server)
        return server->info;

    server = std::make_unique<RunningServer>();
    server->documentRoot.write("index.html",
        std::string("<!doctype html><title>test</title><p>") + indexMarker + "</p>\n");

    ServerOptions options;
    options.port = 0;
    options.documentRoot = server->documentRoot.path().string();
    options.logLevel = "warn";
    configureApp(options);

    drogon::app().registerController(spikeController());

    std::promise<std::uint16_t> listening;
    drogon::app().registerBeginningAdvice([&listening]
    {
        // Queued after configureApp()'s announcement, so the listeners are listening by now.
        drogon::app().getLoop()->queueInLoop([&listening]
        {
            listening.set_value(drogon::app().getListeners().front().toPort());
        });
    });

    server->thread = std::thread([] { drogon::app().run(); });

    std::future<std::uint16_t> port = listening.get_future();
    REQUIRE(port.wait_for(10s) == std::future_status::ready);
    server->info = {port.get(), server->documentRoot.path()};
    return server->info;
}

TestClient::TestClient(std::uint16_t port, const std::string& path)
{
    loop_.run();
    client_ = drogon::WebSocketClient::newWebSocketClient("127.0.0.1", port, false, loop_.getLoop());

    client_->setMessageHandler([state = state_](std::string&& message,
                                                const drogon::WebSocketClientPtr&,
                                                const drogon::WebSocketMessageType& type)
    {
        const std::lock_guard lock(state->mutex);
        if(type == drogon::WebSocketMessageType::Text)
            state->messages.push_back(std::move(message));
        else if(type == drogon::WebSocketMessageType::Close)
            state->closeFrame = true;
        state->changed.notify_all();
    });

    client_->setConnectionClosedHandler([state = state_](const drogon::WebSocketClientPtr&)
    {
        const std::lock_guard lock(state->mutex);
        state->closed = true;
        state->changed.notify_all();
    });

    drogon::HttpRequestPtr request = drogon::HttpRequest::newHttpRequest();
    request->setPath(path);
    client_->connectToServer(request, [state = state_](drogon::ReqResult result,
                                                       const drogon::HttpResponsePtr&,
                                                       const drogon::WebSocketClientPtr&)
    {
        const std::lock_guard lock(state->mutex);
        if(!state->connected)
            state->connected = result;
        state->changed.notify_all();
    });

    std::unique_lock lock(state_->mutex);
    REQUIRE(state_->changed.wait_for(lock, 5s, [&] { return state_->connected.has_value(); }));
    REQUIRE(*state_->connected == drogon::ReqResult::Ok);
}

TestClient::~TestClient()
{
    // The client belongs to its loop, so it is stopped and let go on that loop's thread.
    std::promise<void> released;
    loop_.getLoop()->runInLoop([this, &released]
    {
        client_->stop();
        client_.reset();
        released.set_value();
    });
    released.get_future().wait();
}

void TestClient::send(const std::string& text)
{
    client_->getConnection()->send(text);
}

void TestClient::close()
{
    client_->getConnection()->shutdown();
}

bool TestClient::waitForMessages(std::size_t count, std::chrono::milliseconds timeout)
{
    std::unique_lock lock(state_->mutex);
    return state_->changed.wait_for(lock, timeout, [&] { return state_->messages.size() >= count; });
}

bool TestClient::waitForClose(std::chrono::milliseconds timeout)
{
    std::unique_lock lock(state_->mutex);
    return state_->changed.wait_for(lock, timeout, [&] { return state_->closed; });
}

std::vector<std::string> TestClient::messages()
{
    const std::lock_guard lock(state_->mutex);
    return state_->messages;
}

bool TestClient::sawCloseFrame()
{
    const std::lock_guard lock(state_->mutex);
    return state_->closeFrame;
}

} // namespace whist::test
