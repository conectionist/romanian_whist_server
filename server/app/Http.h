#ifndef WHIST_APP_HTTP_H
#define WHIST_APP_HTTP_H

#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>

#include <string_view>

namespace whist
{

// Whether any segment of a decoded path starts with a dot: .git, .vite, and the . and .. that lead
// out of a folder.
bool hasDotSegment(std::string_view path);

// How long a browser may keep what a successful answer to this path holds.
std::string_view cacheControlFor(std::string_view path);

// Sets the page's headers on a response, and the caching rule on a successful one. A header that is
// already right is not written again, so a response that Drogon keeps and sends many times is only
// read.
void addPageHeaders(const drogon::HttpRequestPtr& request, const drogon::HttpResponsePtr& response);

// {"version", "commit", "protocol"}: what the page's source links and its About page read.
drogon::HttpResponsePtr versionResponse(std::string_view version, std::string_view commit, int protocol);

// 200 {"status":"ok"}, or 503 {"status":"draining"} for a server that has begun to stop.
drogon::HttpResponsePtr healthResponse(bool draining);

} // namespace whist

#endif
