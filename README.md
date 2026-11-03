# Romanian Whist server

The game server for Romanian Whist's online tables, built on the
[Romanian Whist engine](https://github.com/conectionist/romanian_whist_engine) and
[Drogon](https://github.com/drogonframework/drogon). Its clients are the browser page and the
Android app. The page is moving to its own repository,
[`romanian_whist_web`](https://github.com/conectionist/romanian_whist_web). Until it has moved,
it is in `web/` here.

**Status:** a skeleton. The server serves the page and answers on a WebSocket echo at `/ws`.
[`docs/web-client-plan.md`](docs/web-client-plan.md) is the plan for the rest.

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

## Building the page

Node 22.12 or later, in `web/`:

```
npm ci
npm run build
```

## Running it

From the repository's root, after building both:

```
./build/server/app/whist_server --config server/app/config.json
```

Then open http://127.0.0.1:8080. `server/app/config.json` lists every setting with its default.

While working on the page, `npm run dev` in `web/` serves it on port 5173 and passes `/ws` through to
the server on port 8080.

## Tests

```
nice -n 19 ctest --preset default -j 1                     # the server
cd web && nice -n 19 npx vitest run --maxWorkers=2         # the page's unit tests
cd web && nice -n 19 npm run e2e -- --workers=1            # the page against the server, in Chromium
```

The end-to-end tests need Playwright's Chromium once: `npx playwright install chromium`.

## Licence

AGPL-3.0-or-later, as the engine is. A server built from this code must offer its users the source
of what it runs.
