#include "TestSupport.h"

#include "SpikeController.h"
#include "app/Server.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/reporters/catch_reporter_event_listener.hpp>
#include <catch2/reporters/catch_reporter_registrars.hpp>

#include <drogon/drogon.h>
#include <json/json.h>

#include <fstream>
#include <future>
#include <memory>
#include <random>
#include <thread>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
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
    bool started = false;
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

        if(!server->started)
        {
            // It never came up, so quit() would not reach it and a join would hang.
            if(server->thread.joinable())
                server->thread.detach();
            return;
        }

        // The senders go first: a send after quit() would reach an I/O loop that no longer exists.
        spike().joinSenders();
        stopApp();
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

std::uint16_t freePort()
{
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    REQUIRE(fd >= 0);

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    socklen_t length = sizeof address;
    const bool bound = ::bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof address) == 0
                    && ::getsockname(fd, reinterpret_cast<sockaddr*>(&address), &length) == 0;
    ::close(fd);

    REQUIRE(bound);
    return ntohs(address.sin_port);
}

const TestServer& testServer()
{
    std::unique_ptr<RunningServer>& server = runningServer();
    if(server)
    {
        REQUIRE(server->started);
        return server->info;
    }

    server = std::make_unique<RunningServer>();
    server->documentRoot.write("index.html",
        std::string("<!doctype html><title>test</title><p>") + indexMarker + "</p>\n");

    ServerOptions options;
    options.port = freePort();
    options.documentRoot = server->documentRoot.path().string();
    options.logLevel = "warn";
    options.instantFloor = std::chrono::milliseconds(1);

    auto listening = std::make_shared<std::promise<void>>();
    std::future<void> ready = listening->get_future();
    configureApp(options, [listening] { listening->set_value(); });
    drogon::app().registerController(spikeController());

    server->thread = std::thread([] { drogon::app().run(); });

    REQUIRE(ready.wait_for(10s) == std::future_status::ready);
    server->info = {options.port, server->documentRoot.path()};
    server->started = true;
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

void TestClient::sendJson(const Json::Value& message)
{
    Json::StreamWriterBuilder builder;
    builder["indentation"] = "";
    send(Json::writeString(builder, message));
}

namespace
{

Json::Value parseOrNull(const std::string& text)
{
    Json::CharReaderBuilder builder;
    const std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
    Json::Value value;
    std::string errors;
    if(!reader->parse(text.data(), text.data() + text.size(), &value, &errors))
        return Json::Value(Json::nullValue);
    return value;
}

} // namespace

std::vector<Json::Value> TestClient::json()
{
    std::vector<Json::Value> parsed;
    for(const std::string& text : messages())
        parsed.push_back(parseOrNull(text));
    return parsed;
}

std::optional<std::size_t> TestClient::waitFor(const std::function<bool(const Json::Value&)>& match,
                                               std::chrono::milliseconds timeout, std::size_t from)
{
    const auto end = std::chrono::steady_clock::now() + timeout;
    std::unique_lock lock(state_->mutex);
    std::size_t next = from;
    while(true)
    {
        for( ; next < state_->messages.size() ; next++)
        {
            if(match(parseOrNull(state_->messages[next])))
                return next;
        }
        if(state_->changed.wait_until(lock, end) == std::cv_status::timeout && next >= state_->messages.size())
            return std::nullopt;
    }
}

bool TestClient::sawCloseFrame()
{
    const std::lock_guard lock(state_->mutex);
    return state_->closeFrame;
}

} // namespace whist::test
