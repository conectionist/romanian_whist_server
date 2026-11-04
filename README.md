# Romanian Whist server

The game server for Romanian Whist's online tables, built on the
[Romanian Whist engine](https://github.com/conectionist/romanian_whist_engine) and
[Drogon](https://github.com/drogonframework/drogon). Its clients are the browser page, in
[`romanian_whist_web`](https://github.com/conectionist/romanian_whist_web), and the Android app.

**Status:** solo and online games can be played end to end: the server runs them at `/ws`, and a
plain debug page in `server/www/` plays them in a browser, reloads included. People who go away are
replaced by a computer player, and take their seat back when they return. Tables nobody is at are
closed. Not yet: limits on connections and tables, and deployment, so it is not for the public
internet. [`docs/server-plan.md`](docs/server-plan.md) is the plan for the rest. The page's part of
it is in `romanian_whist_web`.

## The protocol

[`protocol/PROTOCOL.md`](protocol/PROTOCOL.md) describes every message, and
[`protocol/fixtures/`](protocol/fixtures/) holds a real sample of each, for clients' tests.

## Getting the code

```
git clone --recurse-submodules https://github.com/conectionist/romanian_whist_server.git
```

Cloned without `--recurse-submodules`? Run `git submodule update --init --recursive`.

## Building the server

Linux, with a C++20 compiler (g++ 13 or clang 18), CMake 3.25 or later, and Drogon's packages:

```
sudo apt install libjsoncpp-dev uuid-dev zlib1g-dev libssl-dev
cmake --preset default
nice -n 19 cmake --build --preset default -j 2
```

The presets are `default` (Debug), `release`, `clang`, `asan` and `tsan`.

## Running it

From the repository's root, after building:

```
./build/server/app/whist_server --config server/app/config.json
```

Then open http://127.0.0.1:8080, which shows the debug page in `server/www/`: pick a preset, edit
the settings if you like, and play or watch a solo game.
`server/app/config.json` lists every setting with its default. To serve the browser page instead,
set `document_root` to its built `dist/` folder.

## Tests

```
nice -n 19 ctest --preset default -j 1
```

The fixtures are checked by the core's tests. After changing a message, rewrite them and commit
the difference:

```
WHIST_WRITE_FIXTURES=1 nice -n 19 ./build/server/tests/core_tests "[fixtures]"
```

The browser tests of the page against this server are in `romanian_whist_web`.

The chaos test starts the server with short timeouts and plays online tables whose people answer,
close their tabs, leave and come back at random. It needs Node 22 or later and no packages, and it
takes a core for as long as it runs, so run it on purpose, and in the background:

```
nice -n 19 node tools/chaos/chaos.mjs --server build/server/app/whist_server --minutes 60
```

It passes when every game has reached its end after the last disruption, no table stalled, and no
snapshot or prompt carried a hand its viewer may not see.

## Licence

AGPL-3.0-or-later, as the engine is. A server built from this code must offer its users the source
of what it runs.
