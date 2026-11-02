#ifndef WHIST_APP_SERVER_H
#define WHIST_APP_SERVER_H

#include "app/ServerOptions.h"

namespace whist
{

// Sets up drogon::app() from the options. Call it once, before app().run().
void configureApp(const ServerOptions& options);

} // namespace whist

#endif
