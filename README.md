# Romanian Whist server

The game server for Romanian Whist's online tables, built on the
[Romanian Whist engine](https://github.com/conectionist/romanian_whist_engine) and
[Drogon](https://github.com/drogonframework/drogon). Its clients are the browser page, in
[`romanian_whist_web`](https://github.com/conectionist/romanian_whist_web), and the Android app.

**Status:** solo and online games can be played end to end: the server runs them at `/ws`, and a
plain debug page in `server/www/` plays them in a browser, reloads included. People who go away are
replaced by a computer player, and take their seat back when they return. Tables nobody is at are
closed. It runs from Docker images, below. Not yet: limits on connections and tables, so it is not
for the public internet. [`docs/server-plan.md`](docs/server-plan.md) is the plan for the rest. The
page's part of it is in `romanian_whist_web`.

## The protocol

[`protocol/PROTOCOL.md`](protocol/PROTOCOL.md) describes every message, and
[`protocol/fixtures/`](protocol/fixtures/) holds a real sample of each, for clients' tests.

## Deploy with Docker (recommended)

Two images, for linux/amd64, are published on GitHub's registry:

- `ghcr.io/conectionist/romanian_whist_server`: the server alone, on Ubuntu 26.04;
- `ghcr.io/conectionist/romanian_whist_web`: Caddy serving the browser page, with automatic HTTPS,
  and passing `/ws` on to the server. It is built in
  [`romanian_whist_web`](https://github.com/conectionist/romanian_whist_web).

Their tags are `latest` (the newest `master`), a release such as `0.1.0` or `0.1`, and
`sha-<commit>`.

**Not for the public internet yet.** The server has no limits on connections, tables or message
rates, which come with phase 9 of [the plan](docs/server-plan.md). Try it on a machine you can
reach, or behind something that limits who connects.

### The full stack

[`deploy/docker-compose.yml`](deploy/docker-compose.yml) runs both images, with Caddy getting the
certificate. You need Docker with Compose, and a domain name that points at the machine, with
ports 80 and 443 open:

```
mkdir whist && cd whist
curl -fsSLO https://raw.githubusercontent.com/conectionist/romanian_whist_server/master/deploy/docker-compose.yml
curl -fsSL -o .env https://raw.githubusercontent.com/conectionist/romanian_whist_server/master/deploy/.env.example
```

Edit `.env`: `WHIST_DOMAIN` is the domain name. To try the stack without a domain, set it to `:80`,
which serves plain HTTP at http://localhost. Then:

```
docker compose pull
docker compose up -d
```

- **Updating:** `docker compose pull && docker compose up -d`. A service whose image is new is
  restarted; the server's restart ends the games that are running.
- **Logs:** `docker compose logs -f server`, or `web`.
- **Stopping:** `docker compose down`. The certificates stay in a volume.
- **Stopping the server** takes a few seconds: on SIGTERM it ends every game and closes every
  connection. The compose file allows it 30 s.
- **Settings:** the image holds `/etc/whist/config.json`, which lists every setting with its
  default, as [`server/app/config.json`](server/app/config.json) does. To change one, take the
  image's file, edit it, and mount it back, with `volumes: [./config.json:/etc/whist/config.json:ro]`
  under `server:` in the compose file:
  ```
  docker run --rm --entrypoint cat ghcr.io/conectionist/romanian_whist_server /etc/whist/config.json > config.json
  ```
  Keep `"address": "0.0.0.0"` and `"port": 8080`: the container's health check and the page's
  Caddy use them. Behind your own proxy, the port on the host is `WHIST_LISTEN` in `.env`.
- **Pairing:** the page and the server speak the same protocol only when their versions match. Each
  image carries the version as the label `io.github.conectionist.whist.protocol`:
  ```
  docker image inspect --format '{{ index .Config.Labels "io.github.conectionist.whist.protocol" }}' IMAGE
  ```
  Pin both tags in `.env` in production, and pick two whose labels are equal. When the protocol
  changes, the server is released first, then a page built against it, and the two are deployed
  together. The page's image also carries the server revision it was tested with, in
  `io.github.conectionist.whist.server-revision`.

### The server alone

For a page of your own, or the debug page, which the image serves at `/`:

```
docker run -d --name whist --restart unless-stopped --read-only --tmpfs /tmp \
    --cap-drop ALL --security-opt no-new-privileges -p 127.0.0.1:8080:8080 \
    ghcr.io/conectionist/romanian_whist_server
```

The page's repository, [`romanian_whist_web`](https://github.com/conectionist/romanian_whist_web),
covers the other case: the page alone, pointed at any server.

## Behind your own reverse proxy

Already running Caddy, nginx or another proxy that holds TLS and ports 80 and 443? Then you do not
want a second Caddy. In this setup the server serves the page itself, on `127.0.0.1:8080`, and your
proxy forwards everything to it.

```
mkdir whist && cd whist
curl -fsSL -o docker-compose.yml https://raw.githubusercontent.com/conectionist/romanian_whist_server/master/deploy/docker-compose.own-proxy.yml
curl -fsSL -o .env https://raw.githubusercontent.com/conectionist/romanian_whist_server/master/deploy/.env.example
docker compose pull
docker compose up -d
```

[`deploy/docker-compose.own-proxy.yml`](deploy/docker-compose.own-proxy.yml) uses the same two
images. A one-shot `page` service copies the built page out of the page's image into a volume, and
exits; the server mounts that volume read-only as its `document_root`. Nothing of ours claims a
port or fetches a certificate. `.env` can set `WHIST_SERVER_TAG`, `WHIST_WEB_TAG` and
`WHIST_LISTEN`, which is where the server listens (`127.0.0.1:8080`). It speaks plain HTTP: use
`0.0.0.0:8080` only for a proxy on another host, over a private network.

Give the game a host of its own, such as `whist.example.org`. A sub-path such as
`example.org/whist` does not work, because the page's links start at the root.

**Caddy.** This needs Caddy 2.9.1 or later (`caddy version` says which you have): an older one
accepts the file but reads its `match` lines as headers, and then puts the year-long cache rule on
error answers too. Copy [`deploy/examples/whist.caddy`](deploy/examples/whist.caddy) to
`/etc/caddy/whist.caddy`, and import it in the site block:

```
whist.example.org {
	import /etc/caddy/whist.caddy
}
```

**nginx.** Copy both files of [`deploy/examples/nginx/`](deploy/examples/nginx/) to
`/etc/nginx/snippets/`, and include the first in the `server` block that holds your TLS settings:

```
server {
    server_name whist.example.org;
    # listen 443 ssl; and your certificate lines
    include snippets/whist.conf;
}
```

The locations of `whist.conf` set their own `add_header` lines, and nginx then drops the ones of the
`server` block, such as HSTS. Put your own headers in `whist-headers.conf`, which every location but
`/ws` includes.

**Another proxy.** Forward everything to `127.0.0.1:8080`, pass `/ws` on as a WebSocket (the
`Upgrade` and `Connection` headers), and add the headers of
[`whist-headers.conf`](deploy/examples/nginx/whist-headers.conf) and the cache rules of
[`whist.caddy`](deploy/examples/whist.caddy), on successful answers only: an error cached for a
year would outlive the fault.

**A proxy in a container** reaches the server over a Docker network, not over the host's port. Put
this in `docker-compose.override.yml` beside the compose file (it needs Compose 2.24 or later), set
`WHIST_PROXY_NETWORK` in `.env` to the network your proxy is on, and use `whist-server:8080` in
place of `127.0.0.1:8080` in the examples:

```yaml
services:
  server:
    ports: !reset []
    networks:
      default: {}
      proxy:
        aliases: [whist-server]
networks:
  proxy:
    name: ${WHIST_PROXY_NETWORK:?the Docker network your proxy is on}
    external: true
```

**Why the examples add headers.** The server does not send a content-security-policy, `nosniff`, a
referrer policy or cache rules yet; phase 9 adds them. The Caddy image sets them for the full stack,
and the examples set them for your proxy, with the same values, so the page behaves the same. Once
the server sends them, the lines in the examples become redundant.

- **Updating:** `docker compose pull && docker compose up -d` runs `page` again, which copies the
  files that changed. The server restarts only when its own image is new, which ends the games that
  are running. The files of the older page stay, so a tab open on it still finds its scripts. To
  clear them: `docker compose down && docker volume rm whist_page`.
- **Not for the public internet yet**, for the reason given above.

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

## Building the image yourself

From a checkout with its submodules, the server's image and a container to try it in:

```
WHIST_COMMIT=$(git rev-parse --short HEAD) docker compose up --build
```

Then open http://127.0.0.1:8080. Or build the image alone:

```
nice -n 19 docker build --build-arg JOBS=2 --build-arg WHIST_COMMIT=$(git rev-parse --short HEAD) \
    -t romanian_whist_server .
```

The first build compiles Drogon too and takes several minutes. `JOBS` is the number of compile jobs,
2 unless you say otherwise, so that the machine stays usable. `WHIST_COMMIT` is what the server
reports as its commit, since the build context holds no `.git`.

## Deploying without Docker

Build the `release` preset as above, set `document_root` in a configuration file to the page's built
`dist/`, run `whist_server --config <file>` under a service manager that restarts it and raises its
file limit, and put a proxy in front that holds TLS and passes `/ws` on. A Caddyfile and a systemd
unit for this come with phase 9; [the plan's §12](docs/server-plan.md#12-deployment-and-operations)
says what they hold.

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

It passes when every table finishes a game after the last disruption, no table stalled, and no
snapshot or prompt carried a hand its viewer may not see.

The smoke test plays the first moments of a solo game against a running server, to see that an image
or a deployment works end to end: the server answers on its page, upgrades `/ws`, greets a new member
with the commit it was built from, and starts a game that asks the player for a bet. A proxy in front
is fine if it passes `/ws` on. It needs Node 22 or later and no packages, and exits 0 on a pass and 1
otherwise:

```
node tools/smoke/smoke.mjs --url http://127.0.0.1:8080 --commit a31743d
```

`--timeout` is the seconds to wait for the server to answer, and again for the game to ask for a
bet; it defaults to 10. Without `--commit` the welcome's commit is not checked.

## The look fixtures

[`protocol/look/`](protocol/look/) holds the states the browser page draws its tables from, to
compare its look with the desktop client's screenshots. A small tool plays the desktop's screenshot
games and writes them. After changing a message, or the desktop's shots, write them again and
commit the difference:

```
nice -n 19 ./build/tools/look/whist_look
```

`core_tests "[look]"` fails when they are stale.

## Licence

AGPL-3.0-or-later, as the engine is. A server built from this code must offer its users the source
of what it runs.
