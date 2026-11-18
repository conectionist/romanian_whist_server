#include "app/Http.h"

#include "app/PageHeaders.h"
#include "core/Codec.h"

#include <json/value.h>

#include <algorithm>
#include <string>

namespace whist
{

namespace
{

void setHeader(const drogon::HttpResponsePtr& response, const std::string& name, std::string_view value)
{
    if(response->getHeader(name) != value)
        response->addHeader(name, std::string(value));
}

bool succeeded(drogon::HttpStatusCode code)
{
    const int number = static_cast<int>(code);
    return (number >= 200 && number < 300) || code == drogon::k304NotModified;
}

drogon::HttpResponsePtr jsonResponse(const Json::Value& body, drogon::HttpStatusCode code)
{
    drogon::HttpResponsePtr response = drogon::HttpResponse::newHttpResponse();
    response->setStatusCode(code);
    response->setContentTypeCode(drogon::CT_APPLICATION_JSON);
    response->setBody(toText(body));
    return response;
}

} // namespace

bool hasDotSegment(std::string_view path)
{
    std::size_t start = 0;
    while(start <= path.size())
    {
        const std::size_t end = std::min(path.find('/', start), path.size());
        if(end > start && path[start] == '.')
            return true;
        start = end + 1;
    }
    return false;
}

std::string_view cacheControlFor(std::string_view path)
{
    if(path.starts_with("/assets/"))
        return "public, max-age=31536000, immutable";
    if(path == "/" || path == "/index.html" || path.starts_with("/t/"))
        return "no-cache";
    if(path == "/version" || path == "/health")
        return "no-store";
    return "public, max-age=3600";
}

void addPageHeaders(const drogon::HttpRequestPtr& request, const drogon::HttpResponsePtr& response)
{
    setHeader(response, "Content-Security-Policy", contentSecurityPolicy);
    setHeader(response, "X-Content-Type-Options", contentTypeOptions);
    setHeader(response, "Referrer-Policy", referrerPolicy);

    // An error is never given a long life, so a fault that is mended is not remembered.
    if(succeeded(response->statusCode()))
        setHeader(response, "Cache-Control", cacheControlFor(request->path()));

    // Drogon dates a static file five seconds ahead, which would contradict the rule above.
    if(!response->getHeader("Expires").empty())
        response->removeHeader("Expires");
}

drogon::HttpResponsePtr notFoundPage()
{
    drogon::HttpResponsePtr response = drogon::HttpResponse::newHttpResponse();
    response->setStatusCode(drogon::k404NotFound);
    response->setContentTypeCode(drogon::CT_TEXT_HTML);
    response->setBody("<!doctype html><html><head><title>404 Not Found</title></head>"
                      "<body><h1>404 Not Found</h1></body></html>\n");
    return response;
}

drogon::HttpResponsePtr versionResponse(std::string_view version, std::string_view commit, int protocol)
{
    Json::Value body(Json::objectValue);
    body["version"] = std::string(version);
    body["commit"] = std::string(commit);
    body["protocol"] = protocol;
    return jsonResponse(body, drogon::k200OK);
}

drogon::HttpResponsePtr healthResponse(bool draining)
{
    Json::Value body(Json::objectValue);
    body["status"] = draining ? "draining" : "ok";
    return jsonResponse(body, draining ? drogon::k503ServiceUnavailable : drogon::k200OK);
}

} // namespace whist
