# Romanian Whist server

The game server for Romanian Whist's online tables, built on the
[Romanian Whist engine](https://github.com/conectionist/romanian_whist_engine) and
[Drogon](https://github.com/drogonframework/drogon). Its clients are the browser page, in
[`romanian_whist_web`](https://github.com/conectionist/romanian_whist_web), and the Android app.

**Status:** version 1.0.1. Solo and online games are played end to end: the server runs them at
`/ws`, and a plain debug page in `server/www/` plays them in a browser, reloads included. People who
go away are replaced by a computer player, and take their seat back when they return. Tables nobody
is at are closed. It limits connections, tables, joins and message rates per address, and drains
before it stops, so a deploy lets the games that are running end. It runs from Docker images, on a
VPS or behind your own proxy, or by hand. [`docs/server-plan.md`](docs/server-plan.md) is the plan.
The page's part of it is in `romanian_whist_web`.

## The protocol

[`protocol/PROTOCOL.md`](protocol/PROTOCOL.md) describes every message, and
[`protocol/fixtures/`](protocol/fixtures/) holds a real sample of each, for clients' tests.

## Deploy with Docker (recommended)

Two images, for linux/amd64, are published on GitHub's registry:

- `ghcr.io/conectionist/romanian_whist_server`: the server alone, on Ubuntu 26.04;
- `ghcr.io/conectionist/romanian_whist_web`: Caddy serving the browser page, with automatic HTTPS,
  and passing `/ws` on to the server. It is built in
  [`romanian_whist_web`](https://github.com/conectionist/romanian_whist_web).

Their tags are `latest` (the newest `master`), a release such as `1.0.0` or `1.0`, and
`sha-<commit>`. Pin both in production.

For a VPS from nothing, see [On a VPS, step by step](#on-a-vps-step-by-step).

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
  restarted; the server drains first (see Stopping), so the games that are running get a chance to
  end, and `up -d` waits for it: up to `drain_ms`, 15 minutes. A quiet hour is the time for it, and
  `tmux` or `nohup` keeps the command alive if your connection drops.
- **Logs:** `docker compose logs -f server`, or `web`. The server writes a line that starts with
  `status` once a minute (`status_log_interval_ms`): connections, tables by state, game threads,
  the games someone is at (`attended_games`, which `max_running_games` limits), those of them at
  Instant (`instant_games`, which `max_instant_games` limits), moves made for players who went
  away, the share of a core it used since the last line, and its processor time and peak memory.
- **Stopping:** `docker compose down`. The certificates stay in a volume.
- **Stopping the server** is a drain. On SIGTERM it takes no new tables or games, tells everyone it
  is restarting, and waits for the games that someone is at to end, for at most `drain_ms` (15
  minutes). A game is attended while a member of its table is connected, and for `grace_period_ms`
  (30 s) after the last one went; a game nobody is at cannot go on, so it is not waited for. When no
  attended game is left, or the time is up, it ends the games that remain, closes every connection
  and exits with code 0. A player who comes back during the drain makes their game count again. A
  second SIGTERM, or Ctrl-C, ends the games at once. The compose files give the container 16 minutes
  to stop, which is longer than `drain_ms`; keep it so if you change either, or Docker kills the
  server in the middle of the drain. The log says each step, and `GET /health` answers 503 from the
  first.
- **Health:** the image checks itself, with `whist_server --health-check`, which asks `GET /health`
  on the server's own address and port and passes on 200 and on 503, since a server that drains is
  alive. `docker compose ps` shows the result. Your own uptime monitor is another matter: see
  [Watching it](#watching-it).
- **Settings:** the image holds `/etc/whist/config.json`, which lists every setting with its
  default, as [`server/app/config.json`](server/app/config.json) does. To change one, take the
  image's file, edit it, and mount it back, with `volumes: [./config.json:/etc/whist/config.json:ro]`
  under `server:` in the compose file:
  ```
  docker run --rm --entrypoint cat ghcr.io/conectionist/romanian_whist_server /etc/whist/config.json > config.json
  ```
  Keep `"address": "0.0.0.0"` and `"port": 8080`: the container's health check and the page's
  Caddy use them. Behind your own proxy, the port on the host is `WHIST_LISTEN` in `.env`.
- **Limits:** `config.json` also holds the limits: messages per second on a connection, connections
  and new tables from one address, joins that found no table from one address, connections in all,
  games running at once that someone is at (`max_running_games`), the ones of those at Instant
  (`max_instant_games`), and tables in all (`max_tables`, which counts a table nobody is at too).
  Past one the server answers `rateLimited`, or `busy` for a game or a table, and the page says so. A game nobody has been at for `grace_period_ms` holds and takes no
  processor, so it does not count, and a player who comes back to their game is never refused.
  An address is the client's: when a connection comes from one of `trusted_proxies`, it is the
  rightmost address in `X-Forwarded-For` that is not in that list, or the leftmost when all are. The
  image trusts the loopback and the private ranges, where Caddy and a proxy on the same host
  connect from, and believes the header from nowhere else.
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

### On a VPS, step by step

A small Linux machine of your own (2 vCPUs and 2 GB do; see [Hardware, and the settings for
it](#hardware-and-the-settings-for-it)) running Ubuntu LTS, a domain name you
control, and a shell on the machine over SSH. `whist.example.org` stands for your domain below.

**1. Docker, from Docker's own apt repository** (these are the steps on
[docs.docker.com](https://docs.docker.com/engine/install/ubuntu/), which is the place to check them):

```
sudo apt-get update
sudo apt-get install -y ca-certificates curl
sudo install -m 0755 -d /etc/apt/keyrings
sudo curl -fsSL https://download.docker.com/linux/ubuntu/gpg -o /etc/apt/keyrings/docker.asc
sudo chmod a+r /etc/apt/keyrings/docker.asc
echo "deb [arch=$(dpkg --print-architecture) signed-by=/etc/apt/keyrings/docker.asc] https://download.docker.com/linux/ubuntu $(. /etc/os-release && echo "${UBUNTU_CODENAME:-$VERSION_CODENAME}") stable" \
    | sudo tee /etc/apt/sources.list.d/docker.list > /dev/null
sudo apt-get update
sudo apt-get install -y docker-ce docker-ce-cli containerd.io docker-buildx-plugin docker-compose-plugin
```

**2. The firewall:** SSH first, so that turning it on does not lock you out, then the web ports.
HTTPS over HTTP/3 uses UDP, so 443 is opened twice.

```
sudo ufw allow 22/tcp
sudo ufw allow 80/tcp
sudo ufw allow 443/tcp
sudo ufw allow 443/udp
sudo ufw enable
```

Docker publishes the ports a compose file names whatever `ufw` says, which is why the stack
publishes only 80 and 443. [`docs/vps-hardening.md`](docs/vps-hardening.md) has a provider's
firewall in place of `ufw`, and SSH with keys only, which the machine needs too.

**3. The domain:** make an `A` record for `whist.example.org` that points at the machine's IPv4
address, and wait until `dig +short whist.example.org` shows it. Caddy asks for the certificate when
it starts, and cannot get it before the name resolves. Do not add an `AAAA` record: the compose
network is IPv4 only, and Docker's userland proxy hands an IPv6 client to Caddy from the bridge's
gateway, which `trusted_proxies` trusts, so every IPv6 client would count as one address. IPv6 needs
`enable_ipv6` on the network and `trusted_proxies` for its range, which this stack does not set up.

**4. The stack:**

```
mkdir whist && cd whist
curl -fsSLO https://raw.githubusercontent.com/conectionist/romanian_whist_server/master/deploy/docker-compose.yml
curl -fsSL -o .env https://raw.githubusercontent.com/conectionist/romanian_whist_server/master/deploy/.env.example
```

Edit `.env`:

```
WHIST_DOMAIN=whist.example.org
WHIST_SERVER_TAG=sha-<the server's commit>
WHIST_WEB_TAG=sha-<the page's commit>
```

Take the two tags from the Packages of each repository on GitHub, and pick a pair whose protocol
labels are equal (see Pairing above). A tag that names a commit never changes under you, which
`latest` does.

```
docker compose pull
docker compose up -d
docker compose ps
```

`ps` shows both services `running`, and the server `healthy` within a few seconds. Open
https://whist.example.org, and check it from another machine with the smoke test:

```
node tools/smoke/smoke.mjs --url https://whist.example.org
```

**5. Updating,** later: change the two tags in `.env`, then `docker compose pull && docker compose up -d`.
That replaces the server, which drains first, so the command can wait up to 15 minutes (`drain_ms`)
for the games that are running to end. Run it in `tmux`, at a quiet hour, and expect the monitor
below to say "down" while it waits: `/health` answers 503 on purpose from the moment the drain
begins.

### Watching it

An uptime monitor from outside the machine tells you when the game cannot be reached, which nothing
on the machine can. Any service that fetches a URL every few minutes will do, UptimeRobot or
another like it:

1. Make an account, and add a monitor of the type **HTTP(s)**.
2. The URL is `https://whist.example.org/health`, and the interval is the shortest the plan allows
   (5 minutes on a free plan). It should expect a 200 answer.
3. Add yourself as the contact: an email, or a message in the app you use.
4. If it can, have it check for the keyword `ok` in the answer, so that a proxy that answers 200 on
   its own is not taken for the game.

`/health` reaches the server through the page's Caddy and through your own proxy, if it forwards
everything, as the examples below do. It answers `{"status":"ok"}`, or 503 `{"status":"draining"}`
while a stop is being waited out. Pause the monitor for a deploy, or expect one alert.

On the machine itself, `docker compose logs server` shows a line that starts with `status` every
minute: connections, tables, game threads, the games someone is at (`attended_games`) and the share
of a core the server used.

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
`0.0.0.0:8080` only for a proxy on another host, over a private network, and never where the open
internet or a network of strangers can reach it. The server believes `X-Forwarded-For` from the
private ranges (`trusted_proxies` in its configuration), so anyone who reaches the port from one
could write any address they like and slip the limits per address. Your proxy must send the header,
once: the nginx example does, and Caddy's `reverse_proxy` does by itself. Under "Another proxy" is
why one line matters.

Give the game a host of its own, such as `whist.example.org`. A sub-path such as
`example.org/whist` does not work, because the page's links start at the root.

**Caddy.** Copy [`deploy/examples/whist.caddy`](deploy/examples/whist.caddy) to
`/etc/caddy/whist.caddy`, and import it in the site block. It is one line, `reverse_proxy`, which
passes the WebSocket on by itself:

```
whist.example.org {
	import /etc/caddy/whist.caddy
}
```

**nginx.** Copy [`deploy/examples/nginx/whist.conf`](deploy/examples/nginx/whist.conf) to
`/etc/nginx/snippets/`, and include it in the `server` block that holds your TLS settings:

```
server {
    server_name whist.example.org;
    # listen 443 ssl; and your certificate lines
    include snippets/whist.conf;
}
```

The file adds no headers of its own, so the ones of the `server` block, such as HSTS, apply as you
set them.

**Another proxy.** Forward everything to `127.0.0.1:8080`, `/health` and `/version` included, pass
`/ws` on as a WebSocket (the `Upgrade` and `Connection` headers), and send `X-Forwarded-For`, which
is how the server tells its clients apart. Send one `X-Forwarded-For` header, that the proxy sets or
extends: Drogon reads the first line only, so a proxy that adds a second line lets each client write
the address the server counts. HAProxy needs `http-request set-header X-Forwarded-For %[src]`. Add
no other headers and no cache rules: the server sends them.

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

**The headers.** The server sends a content-security-policy, `nosniff`, a referrer policy and cache
rules itself: on every answer but the 400, 405 (for a method it does not know) and 413 that its
parser writes, which have no body and no headers. The
cache rules are on successful answers only, and keep `/version` and `/health` out of every cache,
so a new server shows at once. No answer says what server or which version it is: there is no
`Server` header, and the 404 page is a plain one. The examples add nothing, and a proxy that did would send a header
twice.

- **Updating:** `docker compose pull && docker compose up -d` runs `page` again, which copies the
  files that changed. The server restarts only when its own image is new, and drains first. The files of the older page stay, so a tab open on it still finds its scripts. To
  clear them: `docker compose down && docker volume rm whist_page`.
- **Stopping** drains, as for the full stack: the compose file gives the server 16 minutes, and
  `docker compose up -d` can wait that long when it replaces a running server.

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

The server answers two small questions about itself. `GET /version` gives the version, the commit
and the protocol number, as `{"commit": "…", "protocol": 2, "version": "…"}`. `GET /health` gives 200
`{"status":"ok"}` while it runs, and 503 `{"status":"draining"}` once it has been asked to stop: that
is the address for an uptime monitor. Ctrl-C asks it to stop, which waits for the games that
someone is at, up to `drain_ms`; Ctrl-C again stops it at once.

## Building the image yourself

From a checkout with its submodules, the server's image and a container to try it in:

```
WHIST_COMMIT=$(git rev-parse --short HEAD) docker compose up --build
```

Then open http://127.0.0.1:8080. Or build the image alone:

```
docker build --build-arg JOBS=2 --build-arg WHIST_COMMIT=$(git rev-parse --short HEAD) \
    -t romanian_whist_server .
```

The first build compiles Drogon too and takes several minutes. `JOBS` is the number of compile jobs,
2 unless you say otherwise, so that the machine stays usable. `WHIST_COMMIT` is what the server
reports as its commit, since the build context holds no `.git`.

## Deploying without Docker

A small Linux machine of your own, as for the Docker steps above (2 vCPUs and 2 to 4 GB, Debian or
Ubuntu LTS, an `A` record for `whist.example.org`, the firewall open on 22, 80 and 443). The server
runs from a release build, serves the page's built `dist/` itself, and sits behind Caddy, which
holds the certificate. [`deploy/manual/whist-server.service`](deploy/manual/whist-server.service)
and [`deploy/manual/Caddyfile`](deploy/manual/Caddyfile) are the two files this needs.

**1. Build the server,** with the packages of [Building the server](#building-the-server) and the
`release` preset, and configure it afresh on every deploy. The commit the server reports, and the page's
source link follows, is recorded when CMake configures, not when it builds: a build that skips the
configure would report the commit of the last one. Passing it makes it right either way, and it
stays in the build folder's cache, so pass it every time:

```
git pull && git submodule update --init --recursive
cmake --preset release -DWHIST_SERVER_COMMIT=$(git rev-parse --short HEAD)
nice -n 19 cmake --build --preset release --target whist_server -j 2
```

**2. Build the page** from the latest commit of
[`romanian_whist_web`](https://github.com/conectionist/romanian_whist_web) whose submodule points at
this server's commit (`npm ci && npm run build` gives its `dist/`), so that the two speak the same
protocol.

**3. Install them,** with a user of their own that can read them and write nothing:

```
sudo useradd --system --user-group --no-create-home --home-dir /nonexistent --shell /usr/sbin/nologin whist
sudo install -d /opt/whist/bin /opt/whist/www /etc/whist
sudo install -m 0755 build-release/server/app/whist_server /opt/whist/bin/whist_server
sudo rsync -a --delete dist/ /opt/whist/www/
sudo cp server/app/config.json /etc/whist/config.json
```

In `/etc/whist/config.json` set `"document_root": "/opt/whist/www"`. Leave `"address"` at
`127.0.0.1`: Caddy is on the same machine, and the server believes `X-Forwarded-For` from the
loopback only (`trusted_proxies`), so nothing else can write an address for itself.

**4. The service:**

```
sudo cp deploy/manual/whist-server.service /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable --now whist-server
journalctl -u whist-server -f
```

The unit runs it as `whist` with a read-only view of the system, raises the file limit to 65536,
restarts it if it fails, and gives it 16 minutes to stop: a stop is a drain, which waits for the
games someone is at for up to `drain_ms` (15 minutes), as the Docker stops do. `systemctl restart
whist-server` and `stop` wait that long too.

**5. Caddy,** from its own apt repository or your distribution's (any 2.x will do). Put the
Caddyfile in place with your domain in it, check it, and reload:

```
sudo cp deploy/manual/Caddyfile /etc/caddy/Caddyfile
caddy validate --config /etc/caddy/Caddyfile
sudo systemctl reload caddy
```

**6. Check it,** from another machine, and add the monitor of [Watching it](#watching-it):

```
node tools/smoke/smoke.mjs --url https://whist.example.org
```

**Updating:** repeat 1 to 3 for the new commit, then `sudo systemctl restart whist-server`. Install
the binary under a new name and `mv` it over, if the old one is running while you copy.

## Hardware, and the settings for it

The defaults in [`server/app/config.json`](server/app/config.json) are for the machine the load test
ran on: 2 vCPUs and 2 GB. On another machine, start from the row nearest to it below, then run the
load test of [Finding the cap on running games](#finding-the-cap-on-running-games) on it, because
one provider's vCPU is not another's.

|  | vCPUs | RAM | Disk |
|---|---|---|---|
| **Minimum** | 1 | 1 GB | 10 GB |
| **Recommended** | 2 | 2 GB | 20 GB |

Only the recommended machine has been tested. The minimum is an estimate, from the memory and
processor figures below.

Memory is rarely what limits the server: it is about 15 MB when idle, and each table adds 0.06 MB
(solo) to 0.23 MB (online), so 400 tables take about 92 MB. The rest of the RAM is for the system,
Docker and Caddy. Building the server on the machine, as [Deploying without
Docker](#deploying-without-docker) does, needs more than running it: 2 GB, or 1 GB and a swap file.
The processor is what limits it. On the VPS, a game takes about this much of one core:

| Each game | Share of a core |
|---|---|
| Solo, one person and five computer players, at normal speed | 1.3% |
| Online, six people who answer at once | 2.7% |
| Solo at Instant | 18% |

The rows other than 2 vCPUs are scaled from those figures, not measured:

| Machine | `io_threads` | `max_running_games` | `max_instant_games` | `max_tables` | `max_connections` |
|---|---|---|---|---|---|
| 1 vCPU, 1 GB | 1 | 20 | 2 | 200 | 1000 |
| **2 vCPUs, 2 GB (measured, the defaults)** | **2** | **40** | **4** | **400** | **2000** |
| 4 vCPUs, 4 GB | 2 | 80 | 8 | 800 | 4000 |
| 8 vCPUs, 8 GB | 4 | 160 | 16 | 1600 | 8000 |
| 16 vCPUs, 16 GB | 8 | 320 | 32 | 3200 | 16000 |

How each column scales:

- **`max_running_games`:** 20 a vCPU. The load test gave 40 on 2 vCPUs, from the online tables, the
  most expensive kind of game after Instant.
- **`max_instant_games`:** 2 a vCPU, so Instant games take at most about a third of the machine and
  leave the rest for the other games.
- **`max_tables`:** 10 times `max_running_games`. A table stays after its game ends, and a table
  nobody is at stays until its idle time is over. Even 3200 tables take under 1 GB.
- **`max_connections`:** 50 times `max_running_games`, several times the six seats of every running
  game, which leaves room for watchers and for the tables between games. The Docker stack and the systemd unit allow 65536 open files, far more than any row needs.
- **`io_threads`:** about half the vCPUs, from 2 vCPUs up. The I/O threads carry the messages, and the
  computer players think on game threads of their own, at a lower priority.

The limits per address (`messages_per_second`, `message_burst`, `max_connections_per_ip`,
`tables_per_ip_per_hour`, `failed_joins_per_ip`) protect the server from a single client, not from
too many players, so they stay the same on every machine.

With every game at its cap, the 2-vCPU defaults could reach about 85% of the machine: 36 online
tables of people who answer at once and 4 Instant games. People who think before they play use far
less, so real tables do not get there.

## Finding the cap on running games

`max_running_games` is how many games someone is at that the server runs at once before it answers
`busy`; the status line's `attended_games` is the count it compares with, and `game_threads` also
holds the games nobody is at. The default is 40, which the load test below gave on the VPS.

`max_instant_games` (4) is how many of those games may be at Instant speed, which `instant_games` in
the status line counts. An Instant game's computer players move back to
back, so it takes about a sixth of a core, where a game at any other speed takes a few percent: on
the 2-vCPU VPS, 10 Instant tables took 88% of the machine. A game past the cap is refused with
`busy` and `detail` `instant` when it starts, and a solo game that is switched to Instant is refused
the switch and keeps its speed. Four Instant games take about a third of that machine and leave room
for the rest of `max_running_games`; a load run of Instant tables beside normal ones would confirm
it.

The `max_running_games` number is not a guess: [`tools/load/load.mjs`](tools/load/load.mjs) plays
many tables against a server, ramps them in steps, and says how many the machine carries. It needs
Node 22 or later and no packages, and a server running with
[`deploy/load-config.json`](deploy/load-config.json), which is the Docker image's configuration with
every limit per address lifted (the script plays from one address), a status line every 5 s and a
drain of 5 s.

One run is one kind of table, over the steps `10,20,40,60,80,100` (`--steps`), each held for
`--hold` seconds (180) after `--settle` seconds (15) to start its tables:

| `--kind` | Each table |
|---|---|
| `solo` | One scripted person and five Reckoner Gammas, at normal speed |
| `instant` | The same, at Instant speed: the most CPU a table can ask for |
| `online` | Six scripted people, who answer at once |

For each step it prints the time from an answer sent to the echo of the move coming back (p50, p95,
p99 and the largest, in milliseconds), the server's CPU and memory, the game threads, and what went
wrong. The echo waits for the server's own pacing, so an idle server gives about 40 ms: what load
adds to it is what counts. At the end it prints a table of the steps, and the cap: **the largest
step whose p99 is under 250 ms and whose CPU is under 70% of the machine's cores, and that went
without an error from the server, a connection that closed, or a table that would not start**
(`--p99`, `--cpu` and `--cores` change the first two), with every step before it as good, rounded
down to a multiple of 10. The ramp stops by itself at a step with p99 over a second, CPU over 95%,
or a table that would not start. `--json <file>` keeps the numbers.

The CPU and memory come from the server's own status lines, or are read from `/proc` or `docker
stats`. The memory is not the same in each: the status lines give the most the server has held since
it started (`peak MB` in the table), and `/proc` and `docker stats` what it holds when read (`cur
MB`). The table says which under it. The `MB/tbl` column is what each table cost: the memory of the
step above the idle server's, which it reads before the first step, divided by the tables of the
step. At the end it says what `--max-tables` (400, the server's `max_tables`) tables would take on top
of an idle server, so the cap can be checked against the machine's memory. Peak memory never comes
down, so measure on a server that has just started. The idle reading waits for the first status
line, up to `--baseline-wait` seconds (12, for the load configuration's status line every 5 s): a
server that logs less often needs a longer wait, and with none the table has no `MB/tbl` column,
which the run says in a note.

| Option | Where the server is |
|---|---|
| `--server <program> --config <file>` | The script starts it, reads its status lines and stops it |
| `--status-log <file>` | Anywhere: a file that the server's log is written to, read as it grows |
| `--pid <n>` | A process on this machine |
| `--container <name>` | A container, read with `docker stats` on this machine |

With none of them the latency is measured and the CPU is not, so no cap is suggested.

**Here, a short try** against a server it starts itself (copy `load-config.json`, point its
`document_root` at `server/www`, its `address` at `127.0.0.1` and its `status_log_interval_ms` at
1000):

```
nice -n 19 node tools/load/load.mjs --server build/server/app/whist_server --config my-load.json \
    --kind solo --steps 2 --hold 6 --settle 2
```

**On the VPS,** from the folder of the stack, one run at a time and while nobody plays: a run takes
the machine's whole CPU on purpose. The server is restarted with the load configuration mounted, the
script runs in a `node:26` container on the stack's network, under `nice -n 19`, and reads the
server's log from a file:

```
mkdir -p tools/load tools/smoke
base=https://raw.githubusercontent.com/conectionist/romanian_whist_server/master
curl -fsSL -o tools/load/load.mjs $base/tools/load/load.mjs
curl -fsSL -o tools/smoke/smoke.mjs $base/tools/smoke/smoke.mjs
curl -fsSL -o load-config.json $base/deploy/load-config.json
printf 'services:\n  server:\n    volumes:\n      - ./load-config.json:/etc/whist/config.json:ro\n' > docker-compose.load.yml

load="docker compose -f docker-compose.yml -f docker-compose.load.yml"
for kind in solo instant online; do
    $load up -d --force-recreate --wait server
    $load logs -f --no-log-prefix --tail 0 server > server.log &
    follow=$!
    docker run --rm --network whist_default -v "$PWD/tools:/tools:ro" -v "$PWD/server.log:/server.log:ro" \
        node:26 nice -n 19 node /tools/load/load.mjs --url http://server:8080 --status-log /server.log \
        --kind $kind --cores 2
    kill $follow
done
```

Replace `master` with the commit the server runs. Each kind starts on a server of its own, because the
status line gives the most memory the server has held, which never comes down, and the memory per
table is measured above the idle server's. Put the stack back with `docker compose up -d` after the
three runs. The cap that the `solo` and `online` runs agree on, the smaller of them, goes into
`"max_running_games"` in [`server/app/config.json`](server/app/config.json) and
[`deploy/docker-config.json`](deploy/docker-config.json).

The `instant` run suggests `max_instant_games` instead, without rounding to 10, so give it small steps
(`--steps 2,4,6,8,10`). Its number is how many Instant games the machine carries with nothing else
running: leave the rest of the machine for the other games, as the default of 4 leaves about two
thirds of the VPS.

## Tests

```
nice -n 19 ctest --preset default -j 1
```

The fixtures are checked by the core's tests. After changing a message, rewrite them and commit
the difference:

```
WHIST_WRITE_FIXTURES=1 nice -n 19 ./build/server/tests/core_tests "[fixtures]"
```

The load test's arithmetic and its scripted people are tested without a server by
`node --test tools/load/load.test.mjs`.

The browser tests of the page against this server are in `romanian_whist_web`.

The chaos test starts the server with short timeouts and plays online tables whose people answer,
close their tabs, leave and come back at random. It needs Node 22 or later and no packages, and it
takes a core for as long as it runs, so run it on purpose, and in the background:

```
nice -n 19 node tools/chaos/chaos.mjs --server build/server/app/whist_server --minutes 60
```

With `--url ws://127.0.0.1:8080/ws` in place of `--server` it plays against a server of yours, which
needs a short grace period, a pong timeout under 10 s, and limits per address high enough for every
person to come from one address. The program starts with a configuration of its own otherwise.

It passes when, after the last disruption, every table finishes a game, no table went without
progress for longer than a wait of the server's allows, every tab that went silent was closed by the
server, no snapshot or prompt carried a hand its viewer may not see, every message parsed, and the
server sent no error that a person could not have caused. It exits 0 on a pass and 1 otherwise.

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
