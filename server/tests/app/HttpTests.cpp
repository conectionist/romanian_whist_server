#include "RawSocket.h"
#include "ServerProcess.h"
#include "TestSupport.h"

#include "app/Http.h"
#include "app/PageHeaders.h"
#include "core/BuildInfo.h"
#include "core/Protocol.h"

#include <catch2/catch_test_macros.hpp>

#include <drogon/HttpClient.h>
#include <json/json.h>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

using namespace std::chrono_literals;
using namespace whist;
using namespace whist::test;

namespace
{

// The policy as the page's own copies write it.
constexpr const char* pagePolicy =
    "default-src 'self'; script-src 'self'; style-src 'self'; img-src 'self'; font-src 'self'; connect-src 'self'; "
    "object-src 'none'; base-uri 'none'; form-action 'self'; frame-ancestors 'none'";

std::string lowered(std::string text)
{
    std::ranges::transform(text, text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

// An answer read off the wire, for the requests a client would not send as they are.
struct WireAnswer
{
    int status = 0;
    std::map<std::string, std::string> headers;
    std::string body;
};

WireAnswer parseAnswer(const std::string& text)
{
    WireAnswer answer;
    const std::size_t headerEnd = text.find("\r\n\r\n");
    std::istringstream lines(text.substr(0, headerEnd));

    std::string line;
    std::getline(lines, line);
    std::istringstream statusLine(line);
    std::string version;
    statusLine >> version >> answer.status;

    while(std::getline(lines, line))
    {
        if(!line.empty() && line.back() == '\r')
            line.pop_back();
        const std::size_t colon = line.find(':');
        if(colon == std::string::npos)
            continue;

        std::size_t value = colon + 1;
        while(value < line.size() && line[value] == ' ')
            value++;
        answer.headers[lowered(line.substr(0, colon))] = line.substr(value);
    }
    if(headerEnd != std::string::npos)
        answer.body = text.substr(headerEnd + 4);
    return answer;
}

WireAnswer wireGet(std::uint16_t port, const std::string& target)
{
    return parseAnswer(rawRequest(port, "GET " + target + " HTTP/1.1\r\nHost: test\r\nConnection: close\r\n\r\n"));
}

// What every answer that goes through Drogon's sending carries, whatever its status.
void checkPolicy(const drogon::HttpResponsePtr& response)
{
    CHECK(response->getHeader("Content-Security-Policy") == pagePolicy);
    CHECK(response->getHeader("X-Content-Type-Options") == "nosniff");
    CHECK(response->getHeader("Referrer-Policy") == "no-referrer");
}

std::string fileText(const std::filesystem::path& path)
{
    std::ifstream file(path, std::ios::binary);
    std::ostringstream contents;
    contents << file.rdbuf();
    return contents.str();
}

} // namespace

TEST_CASE("The page's policy is the string the page is written for", "[http][headers]")
{
    CHECK(contentSecurityPolicy == pagePolicy);
    CHECK(contentTypeOptions == "nosniff");
    CHECK(referrerPolicy == "no-referrer");
}

TEST_CASE("A path with a segment that starts with a dot is told apart", "[http][dots]")
{
    for(const char* path : {"/.git/config", "/.vite/manifest.json", "/assets/.x", "/.", "/..", "/a/./b", "/a/../b",
                            "/.hidden", "/a/.b/c", "//.x", "/a/b/.well-known/", "/..a", "/assets/.cache/x.json"})
    {
        CAPTURE(path);
        CHECK(hasDotSegment(path));
    }

    // A dot inside a name, or at its end, is an ordinary one.
    for(const char* path : {"/", "", "/assets/x.js", "/a.b/c.d", "/assets/index-ab.min.js", "/t/K7QM4P", "/ws", "/version",
                            "/a./b", "/a/b./c", "/a/", "/a//b"})
    {
        CAPTURE(path);
        CHECK_FALSE(hasDotSegment(path));
    }
}

TEST_CASE("The caching rule of each kind of path", "[http][cache]")
{
    CHECK(cacheControlFor("/assets/index-abc123.js") == "public, max-age=31536000, immutable");
    CHECK(cacheControlFor("/assets/") == "public, max-age=31536000, immutable");
    CHECK(cacheControlFor("/") == "no-cache");
    CHECK(cacheControlFor("/index.html") == "no-cache");
    CHECK(cacheControlFor("/t/K7QM4P") == "no-cache");
    CHECK(cacheControlFor("/version") == "no-store");
    CHECK(cacheControlFor("/health") == "no-store");
    CHECK(cacheControlFor("/cards/AS.webp") == "public, max-age=3600");
    CHECK(cacheControlFor("/avatars.json") == "public, max-age=3600");
    CHECK(cacheControlFor("/fonts/x.woff2") == "public, max-age=3600");

    // Only the paths themselves, not what merely starts like them.
    CHECK(cacheControlFor("/assetsx/a.js") == "public, max-age=3600");
    CHECK(cacheControlFor("/index.html.bak") == "public, max-age=3600");
    CHECK(cacheControlFor("/t") == "public, max-age=3600");
    CHECK(cacheControlFor("/versions") == "public, max-age=3600");
}

TEST_CASE("/version and /health are small JSON documents", "[http][version]")
{
    const drogon::HttpResponsePtr version = versionResponse("1.2.3", "abc1234", 7);
    CHECK(version->statusCode() == drogon::k200OK);
    CHECK(version->contentType() == drogon::CT_APPLICATION_JSON);
    CHECK(std::string(version->body()) == R"({"commit":"abc1234","protocol":7,"version":"1.2.3"})");

    const drogon::HttpResponsePtr ok = healthResponse(false);
    CHECK(ok->statusCode() == drogon::k200OK);
    CHECK(std::string(ok->body()) == R"({"status":"ok"})");

    const drogon::HttpResponsePtr draining = healthResponse(true);
    CHECK(draining->statusCode() == drogon::k503ServiceUnavailable);
    CHECK(draining->contentType() == drogon::CT_APPLICATION_JSON);
    CHECK(std::string(draining->body()) == R"({"status":"draining"})");
}

TEST_CASE("The server tells its version, its commit and its protocol", "[http][version]")
{
    const TestServer& server = testServer();
    const auto [result, response] = fetch(server.port, "/version");

    REQUIRE(result == drogon::ReqResult::Ok);
    CHECK(response->statusCode() == drogon::k200OK);
    CHECK(response->contentType() == drogon::CT_APPLICATION_JSON);

    const std::shared_ptr<Json::Value> body = response->jsonObject();
    REQUIRE(body);
    CHECK(body->size() == 3);
    CHECK((*body)["version"] == std::string(serverVersion()));
    CHECK((*body)["commit"] == std::string(serverCommit()));
    CHECK((*body)["protocol"] == protocolVersion);
    CHECK((*body)["protocol"].isInt());
}

TEST_CASE("/health says ok while the server runs", "[http][health]")
{
    const TestServer& server = testServer();
    const auto [result, response] = fetch(server.port, "/health");

    REQUIRE(result == drogon::ReqResult::Ok);
    CHECK(response->statusCode() == drogon::k200OK);
    CHECK(std::string(response->body()) == R"({"status":"ok"})");
}

TEST_CASE("Every answer that goes through Drogon's sending has the page's headers", "[http][headers]")
{
    const TestServer& server = testServer();

    struct Case
    {
        std::string path;
        drogon::HttpStatusCode status;

        // The Cache-Control a successful answer is given. An error has none.
        std::string cache;
    };
    const std::string year = "public, max-age=31536000, immutable";
    const std::string hour = "public, max-age=3600";
    const std::vector<Case> cases{
        {"/", drogon::k200OK, "no-cache"},
        {"/index.html", drogon::k200OK, "no-cache"},
        {"/assets/x.js", drogon::k200OK, year},
        {"/assets/x.css", drogon::k200OK, year},
        {"/assets/big.js", drogon::k200OK, year},
        {"/pages/x.html", drogon::k200OK, hour},
        {"/cards/AS.webp", drogon::k200OK, hour},
        {"/fonts/x.woff2", drogon::k200OK, hour},
        {"/avatars.json", drogon::k200OK, hour},
        {"/t/K7QM4P", drogon::k200OK, "no-cache"},
        {"/version", drogon::k200OK, "no-store"},
        {"/health", drogon::k200OK, "no-store"},

        // Missing files, a type that is not served, a table link that is no table's, a dot folder, and
        // the WebSocket's path asked for as a page.
        {"/nothing.js", drogon::k404NotFound, ""},
        {"/assets/nothing.js", drogon::k404NotFound, ""},
        {"/photos/x.jpg", drogon::k404NotFound, ""},
        {"/t/bad", drogon::k404NotFound, ""},
        {"/t/K7QM4", drogon::k404NotFound, ""},
        {"/.vite/manifest.json", drogon::k404NotFound, ""},
        {"/ws", drogon::k404NotFound, ""},
    };

    for(const Case& test : cases)
    {
        CAPTURE(test.path);
        const auto [result, response] = fetch(server.port, test.path);
        REQUIRE(result == drogon::ReqResult::Ok);
        CHECK(response->statusCode() == test.status);
        checkPolicy(response);

        if(test.cache.empty())
            CHECK(response->getHeader("Cache-Control").empty());
        else
            CHECK(response->getHeader("Cache-Control") == test.cache);

        // Drogon dates a static file five seconds ahead, and that would say something else.
        CHECK(response->getHeader("Expires").empty());
    }
}

TEST_CASE("A method the route does not take is refused with the policy and no caching", "[http][headers]")
{
    const TestServer& server = testServer();

    // A route with a parameter, such as a table's link, is not found by another method, where one
    // without is refused.
    for(const auto& [path, status] : std::vector<std::pair<std::string, drogon::HttpStatusCode>>{
            {"/version", drogon::k405MethodNotAllowed}, {"/health", drogon::k405MethodNotAllowed},
            {"/t/K7QM4P", drogon::k404NotFound}})
    {
        CAPTURE(path);
        const auto [result, response] = fetch(server.port, path, {}, drogon::Post);
        REQUIRE(result == drogon::ReqResult::Ok);
        CHECK(response->statusCode() == status);
        checkPolicy(response);
        CHECK(response->getHeader("Cache-Control").empty());
    }
}

TEST_CASE("A file that has not changed is answered 304 with the same headers", "[http][headers]")
{
    const TestServer& server = testServer();

    for(const auto& [path, cache] : std::vector<std::pair<std::string, std::string>>{
            {"/assets/x.js", "public, max-age=31536000, immutable"}, {"/index.html", "no-cache"},
            {"/", "no-cache"}, {"/avatars.json", "public, max-age=3600"}})
    {
        CAPTURE(path);
        const auto [first, fresh] = fetch(server.port, path);
        REQUIRE(first == drogon::ReqResult::Ok);
        const std::string modified = fresh->getHeader("Last-Modified");
        REQUIRE_FALSE(modified.empty());

        const auto [second, again] = fetch(server.port, path, {{"If-Modified-Since", modified}});
        REQUIRE(second == drogon::ReqResult::Ok);
        CHECK(again->statusCode() == drogon::k304NotModified);
        CHECK(again->body().empty());
        checkPolicy(again);
        CHECK(again->getHeader("Cache-Control") == cache);
    }
}

TEST_CASE("The WebSocket's handshake answer has the policy and no caching rule", "[http][headers]")
{
    const TestServer& server = testServer();
    const RawSocket socket(server.port);

    const WireAnswer answer = parseAnswer(socket.handshake());
    CHECK(answer.status == 101);
    CHECK(answer.headers.at("content-security-policy") == pagePolicy);
    CHECK(answer.headers.at("x-content-type-options") == "nosniff");
    CHECK(answer.headers.at("referrer-policy") == "no-referrer");
    CHECK_FALSE(answer.headers.contains("cache-control"));
}

TEST_CASE("The refusals Drogon makes before it routes have no body, so nothing in them to sniff or frame", "[http][headers]")
{
    const TestServer& server = testServer();

    // These are written from the parser straight to the socket, past every advice, so they carry no
    // headers. They carry no body either.
    const WireAnswer tooLarge = parseAnswer(rawRequest(server.port,
        "POST / HTTP/1.1\r\nHost: test\r\nContent-Length: 20480\r\nConnection: close\r\n\r\n"));
    CHECK(tooLarge.status == 413);
    CHECK(tooLarge.body.empty());

    const WireAnswer malformed = parseAnswer(rawRequest(server.port, "GARBAGE\r\n\r\n"));
    CHECK(malformed.status == 400);
    CHECK(malformed.body.empty());
}

TEST_CASE("A path with a dot segment is not found, however the dot is written", "[http][dots]")
{
    const TestServer& server = testServer();

    // The decoded path is what both Drogon and the check read: %2e is a dot.
    const std::vector<std::string> targets{
        "/.secret.txt", "/.vite/manifest.json", "/assets/.hidden.txt", "/assets/.cache/x.json", "/pages/.git/x.txt",
        "/%2esecret.txt", "/%2Esecret.txt", "/%2evite/manifest.json", "/%2Evite/manifest.json",
        "/assets/%2ehidden.txt", "/assets/%2Ecache/x.json", "/pages/%2egit/x.txt",
        "/.", "/..", "/%2e", "/%2e%2e", "/%2E%2E/", "/./index.html", "/assets/./x.js", "/assets/../index.html",
        "/assets/%2e%2e/index.html", "/assets/%2E%2E/index.html", "/assets/%2e./index.html",
        "/%2e%2e%2findex.html", "/assets/%2e%2e%2findex.html", "/assets%2f%2e%2e%2findex.html",
        "/assets/x.js/..", "/.secret.txt?x=1", "/.secret.txt#x"};

    for(const std::string& target : targets)
    {
        CAPTURE(target);
        const WireAnswer answer = wireGet(server.port, target);
        CHECK(answer.status == 404);
        CHECK(answer.body.find(sourceMarker) == std::string::npos);
        CHECK(answer.headers.at("content-security-policy") == pagePolicy);
        CHECK_FALSE(answer.headers.contains("cache-control"));
    }
}

TEST_CASE("Paths with a dot that is no segment's start are served, and so is everything else the page uses", "[http][dots]")
{
    const TestServer& server = testServer();

    for(const std::string& target : std::vector<std::string>{
            "/", "/index.html", "/assets/x.js", "/pages/x.html", "/t/K7QM4P", "/assets/x.js?v=.1", "/assets/x.js?.x",
            "/%61ssets/x.js", "/version", "/health"})
    {
        CAPTURE(target);
        CHECK(wireGet(server.port, target).status == 200);
    }
}

TEST_CASE("The WebSocket and a table's link are not touched by the check", "[http][dots]")
{
    const TestServer& server = testServer();

    TestClient client(server.port, "/ws");
    client.send(R"({"type":"hello","protocol":1,"lang":"en"})");
    CHECK(client.waitFor([](const Json::Value& m) { return m["type"] == "welcome"; }, 5s));

    const auto [result, response] = fetch(server.port, "/t/K7QM4P");
    REQUIRE(result == drogon::ReqResult::Ok);
    CHECK(response->statusCode() == drogon::k200OK);
    CHECK(std::string(response->body()).find(indexMarker) != std::string::npos);
}

TEST_CASE("A file over 200 KB is served whole, every time", "[http][sendfile]")
{
    const TestServer& server = testServer();
    const std::string expected = fileText(server.documentRoot / bigFilePath);
    REQUIRE(expected.size() == bigFileSize);

    // Drogon keeps the answer for five seconds, so the later ones come from what it made the first time.
    for(int i = 0 ; i < 3 ; i++)
    {
        const auto [result, response] = fetch(server.port, std::string("/") + bigFilePath);
        REQUIRE(result == drogon::ReqResult::Ok);
        CHECK(response->statusCode() == drogon::k200OK);
        CHECK(response->getHeader("Content-Length") == std::to_string(bigFileSize));
        CHECK(std::string(response->body()) == expected);
    }
}

TEST_CASE("A large file that is replaced under the server is never served cut short", "[http][sendfile]")
{
    const TestServer& server = testServer();
    const std::filesystem::path file = server.documentRoot / "assets" / "changing.js";
    const std::string before = std::string(bigFileSize, 'o');
    const std::string after = std::string(10 * 1024, 'n');

    std::ofstream(file, std::ios::binary) << before;

    // One connection throughout, so that its I/O thread, which holds the answer it made, serves both.
    trantor::EventLoopThread loop;
    loop.run();
    const drogon::HttpClientPtr client = drogon::HttpClient::newHttpClient("127.0.0.1", server.port, false, loop.getLoop());
    const auto get = [&]
    {
        drogon::HttpRequestPtr request = drogon::HttpRequest::newHttpRequest();
        request->setPath("/assets/changing.js");
        return client->sendRequest(request, 5.0);
    };

    const auto [first, old] = get();
    REQUIRE(first == drogon::ReqResult::Ok);
    CHECK(std::string(old->body()) == before);

    // The way a deploy replaces a file: written beside it, and renamed over it.
    std::ofstream(file.string() + ".new", std::ios::binary) << after;
    std::filesystem::rename(file.string() + ".new", file);

    const auto [second, answer] = get();
    REQUIRE(second == drogon::ReqResult::Ok);
    CHECK(answer->getHeader("Content-Length") == std::to_string(answer->body().size()));
    CHECK((std::string(answer->body()) == before || std::string(answer->body()) == after));
}

TEST_CASE("/health says draining from the moment the server is asked to stop", "[http][health][process]")
{
    TempDir dir;
    const std::uint16_t port = freePort();
    dir.write("index.html", "<!doctype html><title>test</title>\n");
    dir.write("config.json", R"({"port": )" + std::to_string(port) + R"(, "shutdown_delay_ms": 1500, "document_root": ")"
                             + dir.path().string() + R"("})");

    ServerProcess server((dir.path() / "config.json").string());
    const bool listening = server.waitForLine("listening on ", 10s).has_value();
    INFO(server.log());
    REQUIRE(listening);

    const WireAnswer running = wireGet(port, "/health");
    CHECK(running.status == 200);
    CHECK(running.body == R"({"status":"ok"})");

    server.signal(SIGTERM);
    REQUIRE(server.waitForLine("shutting down", 2s));

    // The server serves on for the delay, and says what it is doing.
    const WireAnswer draining = wireGet(port, "/health");
    CHECK(draining.status == 503);
    CHECK(draining.body == R"({"status":"draining"})");
    CHECK(draining.headers.at("content-type").starts_with("application/json"));
    CHECK(draining.headers.at("content-security-policy") == pagePolicy);
    CHECK_FALSE(draining.headers.contains("cache-control"));
    CHECK(wireGet(port, "/version").status == 200);

    CHECK(server.waitForExit(5s) == 0);
}
