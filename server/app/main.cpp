#include "app/HealthCheck.h"
#include "app/Server.h"

#include <drogon/drogon.h>

#include <cstdio>
#include <exception>
#include <string_view>

int main(int argc, char* argv[])
{
    // Line by line, so a pipe or the journal sees each log line as it is written.
    std::setvbuf(stdout, nullptr, _IOLBF, 0);

    const char* configPath = nullptr;
    bool healthCheck = false;
    for(int i = 1 ; i < argc ; i++)
    {
        const std::string_view arg = argv[i];
        if(arg == "--config" && i + 1 < argc)
            configPath = argv[++i];
        else if(arg == "--health-check")
            healthCheck = true;
        else
        {
            std::fprintf(stderr, "usage: whist_server [--config <file>] [--health-check]\n");
            return 2;
        }
    }

    try
    {
        const whist::ServerOptions options = configPath ? whist::loadOptions(configPath) : whist::ServerOptions{};

        // Asks a server that is running, instead of being one.
        if(healthCheck)
            return whist::healthCheck(options);

        whist::configureApp(options);
    }
    catch(const std::exception& error)
    {
        std::fprintf(stderr, "whist_server: %s\n", error.what());
        return 2;
    }

    drogon::app().run();
    return 0;
}
