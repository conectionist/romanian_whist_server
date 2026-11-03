# Romanian Whist server

The game server for Romanian Whist's online tables, built on the
[Romanian Whist engine](https://github.com/conectionist/romanian_whist_engine) and
[Drogon](https://github.com/drogonframework/drogon). Its clients are the browser page, in
[`romanian_whist_web`](https://github.com/conectionist/romanian_whist_web), and the Android app.

**Status:** a skeleton. The server serves a static page and answers on a WebSocket echo at `/ws`.
[`docs/server-plan.md`](docs/server-plan.md) is the plan for the rest. The page's part of
it is in `romanian_whist_web`.

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

Then open http://127.0.0.1:8080, which shows the placeholder page in `server/www/`.
`server/app/config.json` lists every setting with its default. To serve the browser page instead,
set `document_root` to its built `dist/` folder.

## Tests

```
nice -n 19 ctest --preset default -j 1
```

The browser tests of the page against this server are in `romanian_whist_web`.

## Licence

AGPL-3.0-or-later, as the engine is. A server built from this code must offer its users the source
of what it runs.
