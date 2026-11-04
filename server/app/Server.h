#ifndef WHIST_APP_SERVER_H
#define WHIST_APP_SERVER_H

#include "app/ServerOptions.h"

#include <functional>

namespace whist
{

// Sets up drogon::app() from the options. Call it once, before app().run(). onListening runs on
// the main loop once every listener accepts connections.
void configureApp(const ServerOptions& options, std::function<void()> onListening = {});

// Ends every game and joins its thread, then quits Drogon. Every way out of app().run() goes
// through here: a game that sent after quit() would reach an I/O loop that no longer exists.
void stopApp();

} // namespace whist

#endif
