#include "TestSupport.h"

#include <catch2/catch_test_macros.hpp>

#include <drogon/HttpClient.h>

using namespace std::chrono_literals;
using namespace whist::test;

TEST_CASE("The server serves its document root's index.html at /", "[static]")
{
    const TestServer& server = testServer();

    trantor::EventLoopThread loop;
    loop.run();
    const drogon::HttpClientPtr client = drogon::HttpClient::newHttpClient("127.0.0.1", server.port, false, loop.getLoop());

    drogon::HttpRequestPtr request = drogon::HttpRequest::newHttpRequest();
    request->setPath("/");
    const auto [result, response] = client->sendRequest(request, 5.0);

    REQUIRE(result == drogon::ReqResult::Ok);
    REQUIRE(response);
    CHECK(response->statusCode() == drogon::k200OK);
    CHECK(response->contentType() == drogon::CT_TEXT_HTML);
    CHECK(std::string(response->body()).find(indexMarker) != std::string::npos);
}

TEST_CASE("A request body over the cap is refused", "[static]")
{
    const TestServer& server = testServer();

    trantor::EventLoopThread loop;
    loop.run();
    const drogon::HttpClientPtr client = drogon::HttpClient::newHttpClient("127.0.0.1", server.port, false, loop.getLoop());

    drogon::HttpRequestPtr request = drogon::HttpRequest::newHttpRequest();
    request->setMethod(drogon::Post);
    request->setPath("/");
    request->setBody(std::string(20 * 1024, 'x'));
    const auto [result, response] = client->sendRequest(request, 5.0);

    REQUIRE(result == drogon::ReqResult::Ok);
    REQUIRE(response);
    CHECK(response->statusCode() == drogon::k413RequestEntityTooLarge);
}
