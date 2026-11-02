#ifndef WHIST_APP_SERVER_H
#define WHIST_APP_SERVER_H

#include "app/ServerOptions.h"

#include <functional>

namespace whist
{

// Sets up drogon::app() from the options. Call it once, before app().run(). onListening runs on
// the main loop once every listener accepts connections.
void configureApp(const ServerOptions& options, std::function<void()> onListening = {});

} // namespace whist

#endif
