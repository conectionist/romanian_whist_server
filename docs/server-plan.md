# Romanian Whist online: the server's implementation plan

> **Status: phases 0 to 3 are complete (2026-11-04). Engine 5.1.0 carries phase 0. This
> repository's skeleton is phase 1, its core, `server/core`, is phase 2, and solo play end to end,
> through `/ws` and a debug page, is phase 3. Phase 4, online tables, is built and reviewed, and
> waits for the owner's approval. Phases 5 to 11 are not built yet.**
>
> The plan spans three repositories. Phase 0 changes the engine,
> [`romanian_whist_engine`](https://github.com/conectionist/romanian_whist_engine). Phases 1 to 5,
> 9 and 11 build this repository, `romanian_whist_server`, which takes the engine as a submodule,
> the way the terminal and desktop clients do. Phases 6 to 8 and 10 build the page, in
> [`romanian_whist_web`](https://github.com/conectionist/romanian_whist_web). Phases 6 and 10
> change this repository too: phase 6 builds the fixture tool (§10.5), and phase 10 compiles
> `server/core` for the browser. Phase 9 changes the page's repository too: its footer's source
> links, its About page, and how it is deployed (§12). The plan was written in the engine's
> `docs/` and moved here on 2026-10-28, once this repository existed.
>
> On 2026-11-03 the owner renamed this repository from `romanian_whist_web` to
> `romanian_whist_server`, because the Android app, and later other clients, will use the same
> server ([D2](#15-decisions-with-defaults)). The page moved to a new repository,
> `romanian_whist_web`, and so did its part of this plan: §8, the page's tests, and phases 6, 7, 8
> and 10. That part is the web repository's
> [`docs/page-plan.md`](https://github.com/conectionist/romanian_whist_web/blob/master/docs/page-plan.md).
> Their places here keep their numbers and point there. The links point at that repository's
> `master`. Until the page's work reaches it, the current page plan is on the web repository's
> parent branch. Phase 6 creates that branch, and its name is written here then. The same day,
> this file was renamed from `docs/web-client-plan.md` to `docs/server-plan.md`.

This plan is for whoever builds it: the owner, or an agent picking the work up cold. It says what
to build, which existing code each part comes from, in what order, and how to tell each phase is
done. Decisions that are still the owner's are collected in
[section 15](#15-decisions-with-defaults), each with the default this plan assumes until he says
otherwise. When a phase is finished, its outcome is written under it, as the desktop client's
redesign plan does.

**Read first, in this order**

1. This file, once, top to bottom.
2. The engine's [README](https://github.com/conectionist/romanian_whist_engine/blob/master/README.md),
   above all "Round types and who may see a hand" and "Threading, stopping, and failure". The
   server described here is the networked client those sections were written for.
3. The desktop client's `docs/QT_CLIENT_PLAN_proposal1.md`: §§3–6 (the game thread, the bridge,
   the move provider, shutdown) and §9 (the snapshot question phase 0 settles). The server repeats
   that design, with a WebSocket where the desktop client has a window.
4. If you are building the page: its own plan,
   [`docs/page-plan.md`](https://github.com/conectionist/romanian_whist_web/blob/master/docs/page-plan.md)
   in `romanian_whist_web`, which says what to read for the look.

---

## Contents

1. [Summary](#1-summary)
2. [What is being built](#2-what-is-being-built)
3. [Phase 0: the engine's part](#3-phase-0-the-engines-part)
4. [The server](#4-the-server)
5. [Drops, replacement and reconnection](#5-drops-replacement-and-reconnection)
6. [The protocol](#6-the-protocol)
7. [Tables, seats and players](#7-tables-seats-and-players)
8. [The browser page](#8-the-browser-page)
9. [Security and fairness](#9-security-and-fairness)
10. [Testing](#10-testing)
11. [Building and running locally](#11-building-and-running-locally)
12. [Deployment and operations](#12-deployment-and-operations)
13. [Phases](#13-phases)
14. [Risks](#14-risks)
15. [Decisions, with defaults](#15-decisions-with-defaults)
16. [References](#16-references)

---

## 1. Summary

**What.** Romanian Whist in a browser, in two modes:

- **Solo:** you against computer players, with everything the desktop client offers: 2 to 6
  seats, the nine computer players, both game shapes, Forehead and Hidden rounds, pacing, fixed
  deals, watching a game of computer players, English and Romanian.
- **Online:** a player creates a table and shares a link, and others join it. Any seat can be a
  human or a computer player. When a human drops out, a computer player takes over their seat, and
  hands it back when they return.

**How.** One C++ server, built on [Drogon](https://github.com/drogonframework/drogon), runs every
game, solo games included. Each game runs the engine on a thread of its own, as the desktop client
does, and talks to the browsers over one WebSocket per browser tab, in JSON. The page in the browser
is written in TypeScript and looks like the desktop client. The mock-up's HTML, CSS and script are
reused only where they already match the desktop client. The page applies no rule of the game: what
it may show, and what the player may do, come from the server.

**Six rules.** Each later section works out one of these.

1. **The engine is read only on its game thread.** What leaves that thread is a snapshot, already
   filtered for whoever will see it.
2. **A card a player may not see never leaves the server.** The engine filters hands per viewer
   before anything is serialised ([§3.1](#31-e1-a-snapshot-filtered-for-its-viewer)).
3. **Every seat has a computer player behind it from the first deal.** A human seat's computer
   player watches the whole game and decides only while its human is gone. That is the whole of
   "a dropped player is replaced" ([§5](#5-drops-replacement-and-reconnection)).
4. **Every wait has a deadline or a cancel.** No player, connection or bug can stall a table
   ([§4.6](#46-threads-and-locks)).
5. **Events animate; snapshots are the truth.** Each game message carries the whole state after
   the event, as that viewer may see it. The page animates the event, then shows the state. It
   never works state out for itself.
6. **The desktop client decides how the page looks.** Where the desktop client and the mock-up
   differ, the desktop client wins, even when that means writing new code instead of reusing the
   mock-up's ([the page's plan, §2](https://github.com/conectionist/romanian_whist_web/blob/master/docs/page-plan.md#2-the-look-the-desktop-client-wins)).

**Order of work.** The engine first (phase 0). Then the server and a plain debug page, until solo
play, online play, drops and reconnection all work (phases 1–5). Then the desktop client's look
(phases 6–8), then deployment (phase 9). An offline solo mode and game records are optional extras
(phases 10–11). [Section 13](#13-phases) has the phases; the page's, 6 to 8 and 10, are in its
own plan.

---

## 2. What is being built

### 2.1 Carried over from the desktop client

| Desktop client | Web |
|---|---|
| 2–6 seats | Same, solo and online |
| Nine computer players: Random, First card, Low risk, Ducking, Cyborg, Reckoner Alpha to Delta | Same, chosen per seat |
| Play, or watch computer players | Solo: both. Online: everyone plays (spectators later, [§2.3](#23-not-in-the-first-version)) |
| 1-8-1 and 8-1-8 | Same |
| Forehead and Hidden rounds: one of each at the end, or every one-card round Forehead | Same |
| Rules enforced: illegal cards greyed out, the forbidden bid unavailable | Same, from the server's prompt |
| Scoreboard with the RUN column, round sheet, round history, last trick, final results | Same |
| Bot speed (five stops, Slow to Instant), "Wait for me after each round" | Bot speed: per table, applied by the server. The wait: solo as on the desktop; online it becomes the round hold ([§4.8](#48-pacing-the-round-hold-and-pause)) |
| Animation speed (Off to Slow) | Per viewer, applied by the page |
| Pause | Solo only |
| Fixed deals (a seed) | Solo only. Online, a seed would reveal the deal ([§9](#9-security-and-fairness)) |
| English and Romanian | Same, chosen by each viewer |
| Seat portraits and computer players' names from the catalogues | Same catalogues: `avatars.json`, `player_names.json` |

### 2.2 New for the web

- **Online tables.** Create a table, share its link or six-character code, wait in the table's
  lobby, and start once every seat is a human or a computer player.
- **Mixed seats.** Any seat can be a human or a computer player, with the computer player chosen
  per seat, as in the desktop client's New Game.
- **Replacement.** A human who drops is replaced by a computer player after a grace period, and
  gets the seat back on returning. The table chooses which computer player; the default is
  Reckoner Beta.
- **A turn timer** online. A connected player who does not move in time has that one move made
  for them.
- **Resuming after a reload**, solo and online.
- **Each viewer in their own language** at a shared table. The protocol carries data, never
  sentences.

### 2.3 Not in the first version

- **Accounts and passwords.** A player is a display name and a random token kept in the browser
  ([§9](#9-security-and-fairness)).
- **A public list of tables.** Tables are private; you join by link or code.
- **Spectators** at someone else's table, **chat** and **rankings**.
- **A layout for phones held upright.** Phones held sideways work
  ([the page's plan, §11](https://github.com/conectionist/romanian_whist_web/blob/master/docs/page-plan.md#11-keyboard-screen-readers-touch-and-small-screens)).
- **Taking a computer player's seat** in a game that has already started, other than reclaiming
  your own. The seat drivers make it cheap to add later ([§5.5](#55-edge-cases)).
- **Offline play and game records:** optional phases 10 and 11.

---

## 3. Phase 0: the engine's part

Two changes are needed (E1, E2), one is optional (E3), and four things the engine might seem to
need, it does not ([§3.4](#34-what-the-engine-does-not-need)).

### 3.1 E1: a snapshot filtered for its viewer

Two clients keep their own copies of the snapshot code today: the terminal's `GameView` and the
desktop client's `GameSnapshot` with `refreshFromEngine()`. The desktop plan's §9 left open
whether the half that is read straight from the engine belongs in the engine, and recommended
promoting it once there were two real implementations to compare. A third client settles it.

Add to the engine a plain-data `romanian_whist::GameSnapshot` (header `romanian_whist/Snapshot.h`)
and

```cpp
GameSnapshot snapshotFrom(const GameEngine& engine, std::optional<Seat> viewer);
```

**The viewer is a parameter so that a snapshot built without deciding who will see it cannot be
written.** This is the README's own advice to a networked client ("route every hand read through
the viewer"), moved into the engine where every client gets it. `std::nullopt` is a spectator, who
sees no hand at all: the desktop client's rule for a watched game.

The fields are the left column of the desktop plan's §9 table, with the traps the desktop client
found on the way:

| Field | Read from | Note |
|---|---|---|
| status, phase | `getStatus()`, `getPhase()` | The phase as the engine reports it, never inferred |
| round index and count, trick count, trick number, round type | the matching accessors | Index 0-based; clients add 1 to display it |
| trump | `getCurrentTrumpCard()` | Empty in 8-card rounds |
| round leader | `getRoundLeaderSeat()` | Bidding order runs from here |
| active seat | `getActiveSeat()` | Empty between turns |
| forbidden bid | `getForbiddenBet()` | Public: anyone can work it out from the bids |
| cards on the table, each with its seat and whether it is winning | `getCurrentTrick()`, `getCurrentTrickLeader()` | "Winning" is the engine's ranking, never recomputed |
| who led this trick, the suit led | the trick's first card; `getTrickLeaderSeat()` only while the trick is empty | By `onTrickWon()`, `getTrickLeaderSeat()` has moved to the winner (desktop `Snapshot.cpp`, `rebuildTable()`) |
| per seat: name, bidding order, bid (optional), tricks won, cards left, round score, total score, win and loss streaks | the matching accessors | |
| per seat: hand, in the engine's order | `Player::getHand()`, **only when `canSeeHand(viewer, seat)`** | Absent, not present and flagged hidden. The only place in the function that reads a hand |
| bids placed, their sum | derived from the bids | |

Left out: anything about display (the order a hand is shown in, wording, highlights) and anything
the engine does not model (which seats are human, connections, portraits). Hand order stays with
the clients, as the desktop plan's §9 decided.

Keep it plain data and copyable: clients hand it to other threads. `Seat` has no default
constructor, so seats inside the snapshot are optionals or plain indices. No JSON: the engine
takes no dependencies.

**Tests**, in the engine repository:

- Pinned values at known points of a seeded game: after `start()`, mid-bidding, mid-trick, inside
  `onTrickWon()`, after the last round is scored. This is the test the desktop plan's §9 proposed.
- **The leak test.** A property test over many seeded games, at every table size, with Forehead
  and Hidden rounds: at every observer callback, for every viewer (each seat, and the spectator),
  every hand in the snapshot belongs to a seat that viewer may see by `canSeeHand()`, and the
  viewer's own hand is there exactly when `canSeeHand(viewer, viewer)` is true.
- Inside `onTrickWon()`, the table still holds the finished trick, and "led by" names who led it,
  not who won it.

**Release:** additive, so 5.1.0, with a CHANGELOG entry and a README section, "Snapshots for
another thread or another machine", next to the threading section's own `snapshotFrom` sketch.
The desktop and terminal clients are not asked to switch. They can adopt it when it suits them;
the desktop client's `tests/SnapshotTests.cpp` is what would show that the two agree.

### 3.2 E2: strategies standing in for a seat they do not play

The server gives every human seat a computer player that watches the whole game and decides only
while its human is gone ([§5](#5-drops-replacement-and-reconnection)). That relies on something the
code already does but the documentation does not promise: **a Cyborg or a Reckoner given a seat
with `setSeat()` and registered as an observer before `start()` can be asked for a bid or a card at
any point in any round**, including partway through a round in which it made none of that seat's
earlier decisions.

Why it already holds:

- Both strategies learn the game only through the observer callbacks: round starts, every seat's
  bids, every card played, every trick won. A bid the human made reaches the stand-in exactly as
  one it made itself would.
- Both read their own hand from the context of each decision, not from memory.
- The only state either keeps from its own earlier decisions is small, and empty is a valid value
  for it. The Cyborg's `plannedLead` (set when it bids) and `cashingSuit` (set when it leads a
  cash) are both cleared at every round start, so after a takeover they are what they would be at
  the start of a round.
- `CyborgStrategy::setSeat()` already says it is "the way to register a Cyborg that is watching a
  seat it does not occupy".

The stateless strategies (Random, First card, Low risk, Ducking) decide from the context alone and
need nothing.

**Work:**

- **Promise it.** A README section, "A strategy for a seat it does not play", and the same sentence
  on `ReckonerStrategy::setSeat()`.
- **Pin it** in a new `tests/StandbyTests.cpp`. For the Cyborg and each Reckoner preset: many
  seeded games in which seat k is played by another strategy while a stand-in watches it, and at a
  random decision the stand-in takes the seat over for the rest of the game. Every answer must be
  legal (`isBetLegal()`, the legal set), every game must finish, and the Cyborg's
  `getFallbacksTaken()` must stay 0, which says its memory agreed with every position it was shown.
  Cover a takeover during bidding, at a trick's lead, mid-trick, and in a Forehead and a Hidden
  round.
- **Optional:** make `ReckonerStrategy::onGameStarted()` refuse a name that matches no seat, as the
  Cyborg's does. Today it plays as seat 0, which the Cyborg's comment points out. The server does
  not need this, because it always calls `setSeat()`.

### 3.3 E3 (optional): an Emscripten build in CI

Only phase 10 needs the engine compiled to WebAssembly. A CI job that builds the engine and its
tests with Emscripten and runs them under Node would keep it portable in the meantime. It is cheap
insurance, and safe to skip if phase 10 is not wanted. Two things to know: C++ exceptions need a
compiler flag under Emscripten (`-fexceptions` or `-fwasm-exceptions`), and the Reckoner's exact
score test is already GCC-only (see the README).

### 3.4 What the engine does not need

- **A non-blocking `playStep()`.** It would remove the thread per game, and the need for JSPI in
  phase 10. `GameEngine.h` notes that the round loop keeps its state in members precisely so that
  adding one would be an additive change. It is not needed: a parked thread per table is cheap at
  this scale ([§4.10](#410-capacity)). Revisit if tables ever number in the thousands.
- **Saving and restoring a game.** A server restart ends the games in progress. Phase 11 recovers
  them without changing the engine, by replaying a log: the engine is deterministic given the
  shuffle seed and the decisions.
- **An API for replacing a seat's player.** Replacement happens inside the server's move provider;
  the engine sees one provider per seat, as always.
- **JSON, networking, player profiles.** The engine stays free of dependencies.

---

## 4. The server

### 4.1 One server for both modes

Solo games run on the server too, as a private table with one human seat. That gives one code
path and one protocol, and the page cannot tell the modes apart. The costs: a solo game needs a
connection, and a server restart ends it. Phases 10 and 11 deal with those, if they turn out to
matter.

Running the engine in the browser for solo games (WebAssembly) stays possible as phase 10. The
server's session code is written against two small seams, a transport and a way to wait, so it
can be compiled for a Web Worker without touching the game logic.

### 4.2 Why Drogon, and what it does not do

Why:

- **It is C++ and CMake.** The server links the engine and the session code directly, with no
  bindings and no second language on the server.
- **One library** covers WebSockets, static files and timers, and brings jsoncpp, which reads the
  server's own settings file. It is MIT-licensed and maintained (1.9.14, 12 October 2026). The
  signal handlers are the server's own, not Drogon's ([§12](#12-deployment-and-operations)).
- **`WebSocketConnection::send()` may be called from any thread.** Drogon's network layer,
  trantor, queues the write onto the connection's own I/O thread, so a game thread sends directly.
  Phase 1 found two limits:
  - **Order.** A send made on the connection's own I/O thread goes out at once, ahead of sends
    other threads have queued, so the `Outbox` queues every send ([§4.4](#44-the-objects)).
  - **Only `send()`.** Nothing else that reads a connection's state may be called from another
    thread: `connected()` and `disconnected()` race with the I/O thread that sets them.

What it does not do: Drogon's strength is asynchronous I/O and coroutines, and the engine is
synchronous. `run()` blocks inside a move provider while a human thinks. So each game runs on a
thread the server owns, outside Drogon's event loops, and Drogon only carries messages. Any C++
WebSocket server would do the same job; Drogon is the one with the fewest pieces to add.

Pin Drogon 1.9.14 as a git submodule (`libs/drogon`, trantor inside it) and build it with the
project, with its optional parts off: the ORM and database drivers, `drogon_ctl`, the examples.
Check the option names in its `CMakeLists.txt` at the pinned tag. The Ubuntu 24.04 package is
1.8.7, from 2023, too old to pin a new project to. System packages it needs:
`libjsoncpp-dev uuid-dev zlib1g-dev libssl-dev`.

### 4.3 Repository layout

```
romanian_whist_server/              AGPL-3.0-or-later, like the engine
├── libs/RomanianWhistEngine/       submodule, pinned to a release tag (5.1.0 or later)
├── libs/drogon/                    submodule, pinned to v1.9.14
├── server/
│   ├── core/                       whist_session: tables, seats, games, JSON. No Drogon.
│   ├── app/                        the Drogon program: WebSocket controller, config, main()
│   ├── tests/                      Catch2, against core/ through an in-memory transport
│   └── www/                        served by default: a placeholder, then phase 3's debug page
├── protocol/
│   ├── PROTOCOL.md                 every message, kept beside the code
│   ├── fixtures/                   sample messages written by server tests, read by clients' tests
│   └── look/                       the fixture tool's snapshots, for the page's comparisons (§10.5)
├── tools/                          the fixture tool for the page's screenshots (§10.5)
├── deploy/                         Caddyfile, systemd unit
├── docs/
└── CLAUDE.md                       the owner's machine rules (§11)
```

The `server/core` line matters most: everything that decides anything lives where a test can
reach it without a socket. The page's repository has a layout of its own
([the page's plan, §4](https://github.com/conectionist/romanian_whist_web/blob/master/docs/page-plan.md#4-code-layout)).

### 4.4 The objects

| Object | One per | Does |
|---|---|---|
| `Registry` | server | Finds tables by code and members by token; runs the reaper ([§4.9](#49-a-tables-life)) |
| `Table` | table | Holds the settings, seats, members and host, the table's state (Lobby, Playing, Finished, Closed) and the current `GameRun` |
| `Member` | person at a table | Holds a token, a profile (name, gender, portrait), a seat, and the current connection, held weakly |
| `GameRun` | game | Owns the engine, the seat drivers, the publisher and the game thread |
| `SeatDriver` | seat | Asked for moves on the game thread, answered from I/O threads. Owns the seat's computer player and, for a human seat, the link to its human: prompt, deadline, presence |
| `SeatProvider` | seat | What the engine holds: ten lines that forward to the `SeatDriver` (the desktop client's `SeatProvider`, for the same reason, [§4.5](#45-lifetimes-and-declaration-order)) |
| `Publisher` | game | The engine observer. Builds a snapshot per viewer, sends events, paces the game, runs the round hold, and caches what a reconnecting viewer needs |
| `Outbox` | server | `send(member, json)`. Finds the member's live connection and drops the message if there is none. From any thread, the I/O thread included, it stamps `seq` and posts the send to that connection's I/O loop in one step, so messages leave in `seq` order. Whether a member is connected comes from the server's own records, never from asking the connection. Tests use an in-memory `Outbox` that records everything |

### 4.5 Lifetimes and declaration order

The engine owns its move providers and destroys them on the game thread. So, as the desktop client
does (`QtMoveProvider.h`), the engine is handed `SeatProvider`s that own nothing, and `GameRun`
owns the real `SeatDriver`s.

`GameRun`'s members, in declaration order. Destruction runs in reverse, and the order is the
design:

1. The seat drivers, each owning its computer player, which is registered as an observer when it
   is one.
2. The publisher, also an observer.
3. The engine.
4. The thread (`std::jthread`), last, so that it is joined first. The engine, and everything it
   points at, outlive the thread that uses them.

Observers must outlive the engine (README); putting 1 and 2 before 3 is that rule.

### 4.6 Threads and locks

The threads:

- Drogon's I/O threads (two are plenty).
- Drogon's main loop, which runs the reaper's timer.
- One game thread per running game.
- One joiner thread, which joins finished game threads, so that no I/O thread ever waits on a join.

The rules:

1. **The engine is read only on its game thread**, inside callbacks and providers. Off that thread
   there is only what the publisher cached.
2. **I/O threads never wait for a game.** A message from a player takes the table's lock, updates
   state, calls `SeatDriver::submit()` (a mutex and a notify, like the desktop client's
   `QtMoveProvider::submit()`) and returns.
3. **A game thread never waits for an I/O thread.** It sends through the `Outbox`, which queues.
4. **Locks are taken in one order: `Registry`, then `Table`, then `SeatDriver`, then the
   publisher's cache.** Never the reverse. No lock is held while sending, while calling into the
   engine, or while a computer player decides.
5. **Every wait on a game thread has a deadline or a cancel.** The seat driver's wait has the turn
   timer, the grace period and abandonment. The publisher's pacing has cancellation and the round
   hold's timeout. The desktop client learned this with `GameBridge::setPaused()`: a wait with no
   deadline can be ended only by the cancel, so the closing path must cancel unconditionally.
6. **Game threads run at a lower priority than the I/O threads**, set with `setpriority` when the
   thread starts, so a table of Reckoners never makes the server slow to answer.
7. **Every game thread is joined before `app().quit()`.** Each I/O loop is destroyed with its
   thread, and a connection keeps only a plain pointer to its loop. So a send after `quit()`, or
   the last reference to a connection let go after it, reaches freed memory.

Shutting a game down, when a table closes or the server drains:

1. Mark the game as closing.
2. `abandon()` every seat driver. A parked one throws `GameAbandoned`, as the desktop client's
   does.
3. Cancel pacing, and call `requestStop()` for a game that is not parked.
4. Hand the thread to the joiner.

These are the two halves the README describes under "Abandoning a game whose provider is parked".

### 4.7 A seat's driver

Every seat has a `SeatDriver`. It holds:

- **A computer player.** For a computer seat, the strategy chosen for it; for a human seat, the
  table's replacement strategy. It is built before `start()`. When it is a Cyborg or a Reckoner, it
  gets `setSeat(Seat{i})` and is registered as an observer
  ([§3.2](#32-e2-strategies-standing-in-for-a-seat-they-do-not-play)), so it watches every game
  from the first deal.
- **For a human seat, the link:** the member; whether they are here, away or replaced
  ([§5.1](#51-words)); the turn timer; and how many moves in a row were made for them.

`makeBet()` and `playCard()`, on the game thread:

```
if the seat has no human, or its human is replaced:
    return the computer player's decision        (the publisher has already paced it)

build the prompt from the context: the hand in the engine's order, the legal indices,
    the forbidden bid, whether the round is blind
store it as pending, under a new generation number, and send it to the member
wait until one of: a valid answer for this generation | the deadline | abandonment

    answer        -> clear pending, reset the count of moves made for them, return it
    abandonment   -> throw GameAbandoned
    deadline      -> clear pending, tell the member the move was made for them,
                     return the computer player's decision;
                     mark the seat replaced if the member is away,
                     or if this was the third move in a row made for a connected member
```

The deadline is the earlier of the turn timer (online, while the member is connected) and the end
of the grace period (while they are away). Solo games have no turn timer and no replacement
([§5.4](#54-solo-is-different)).

Answers are untrusted. Port the desktop provider's checks, all of which `QtMoveProvider.cpp`
explains at length:

- **The generation gate.** A stale or doubled answer is dropped.
- **Range and legality.** The bid's range and the forbidden bid; the card's index and its
  legality. An invalid answer re-prompts instead of reaching the engine, which would throw out of
  `run()` and end the game. The deadline keeps running, so a client that sends nonsense for ever is
  answered by its computer player.
- **No lock while sending**, and the answer is cleared as it is taken, or the next prompt would
  return it without waiting.

### 4.8 Pacing, the round hold and pause

The publisher paces the game on the game thread, as `GameBridge::pace()` does: a beat before a
computer player's move, and a dwell on a finished trick and on a scored round. Port `Pacing` and
`newgame::pacingFor(botSpeed)` from the desktop client's `src/ui/NewGameDialog.cpp` unchanged: five
speeds, Slow to Instant. Give Instant a small floor on the server (some tens of milliseconds a
move) so that a watched game of computer players cannot keep a core busy for its whole length.

A computer player standing in for a human is paced as a computer player. Where the desktop client
asks `isHuman(seat)`, the server asks the seat driver whether a human is in charge right now.

- **Solo:** exactly the desktop client. The speed and "Wait for me after each round" come from the
  settings and can change mid-game. Pause holds the game for as long as it is on (cancel-safe,
  rule 5).
- **Online:** the speed is a table setting, fixed at the start. "Wait after each round" becomes
  the **round hold**: the round sheet stays up until every connected human has pressed Continue,
  or for 30 s, whichever comes first. There is no pause: one player could hold up everyone, and
  the round hold is the breather.
- **Animation speed** is not the server's business. It is each viewer's setting, applied by the
  page ([the page's plan, §5](https://github.com/conectionist/romanian_whist_web/blob/master/docs/page-plan.md#5-the-playback-queue)).

### 4.9 A table's life

```
Lobby ──start──> Playing ──last round scored──> Finished ──new game──> Playing ...
  │                 │                               │
  └─────────────────┴─────── reaper or host ───────┴──> Closed
```

- **Lobby.** Members join and choose seats and profiles; the host sets up the seats and settings
  and starts the game. If the host leaves, the human who has been there longest becomes host; if
  no human is left, the table closes after 10 minutes.
- **Playing.** One `GameRun`. While no human at the table is connected, the game holds (there is
  no point in computers playing for nobody). The table closes after 10 minutes unless someone
  returns.
- **Finished.** The standings are shown, and the host can start a new game with the same seats: a
  new `GameRun` at the same table, so nobody needs a new link.
- **The reaper** runs every few seconds on Drogon's main loop. It closes connections that have
  stopped answering pings ([§5.1](#51-words)), ends grace periods that nothing else would notice,
  closes idle tables, and hands finished threads to the joiner.

### 4.10 Capacity

These are estimates; phase 9 measures them.

- **A game thread spends its life parked, or asleep between beats.** CPU goes to the Reckoners. By
  their design document's estimates, the Gamma preset takes up to about 80 ms for a bid and 1 to
  60 ms for a card.
- **One game's cost.** A six-player 1-8-1 game is 32 rounds: about 190 bids and 700 cards. With
  six Gammas that is 15 to 20 s of CPU, over a game lasting 13 minutes or more at normal speed, so
  about 2% of one core.
- **Tables of humans cost almost nothing:** their stand-ins only watch.
- **What a small server runs.** A 2-vCPU server runs on the order of a hundred tables of
  Reckoners, and many more ordinary tables.
- **Memory** is thread stacks (reserved, barely touched) and kilobytes of game state.

Configure a maximum number of running games, rather than relying on the CPU running out first.

---

## 5. Drops, replacement and reconnection

### 5.1 Words

| Word | Means |
|---|---|
| **here** | The member's WebSocket is open and answering pings |
| **away** | The connection closed, or stopped answering pings for 30 s. The seat is still theirs |
| **grace period** | 30 s from going away. If their turn comes inside it, the table waits for them, and the others see a countdown |
| **replaced** | The grace period ran out, or they left, or the turn timer ran out three times in a row. Their seat's computer player makes their moves |
| **reclaimed** | They came back. The next decision is theirs again |

**Noticing a vanished connection.** The server pings every 10 s
(`WebSocketConnection::setPingMessage`) and records each pong. Drogon's connection passes pong
frames on to its message callback. A connection with no pong for 30 s is closed and counts as away.
Browsers answer pings below the page's JavaScript, so a tab in the background, whose timers the
browser slows down, still counts as here. Drogon's own idle timeout cannot do this job: the pings
are writes, and writes keep a connection active. At the pinned version, pongs do reach the
controller's `handleNewMessage`, as phase 1's spike showed.

### 5.2 Timelines

1. **A reload, or a short network blip.** Away, then back within 30 s. The others see "Ana is
   reconnecting…" for a moment; if Ana's turn came, the table waited.
2. **Gone for good, during someone else's turn.** Away, then replaced 30 s later; the others see
   "Computer playing for Ana". Her computer player makes every move from then on, and the game goes
   on.
3. **Gone during her own turn.** The table waits for what is left of the grace period. Then her
   computer player makes the move, and the seat is replaced.
4. **Here, but not playing** (a tab left open). The turn timer, 60 s by default, runs out, and her
   computer player makes that one move ("made for you"). Three in a row, and she is treated as gone
   ([timeline 2](#52-timelines)). Anything she then does reclaims the seat.
5. **Leaving on purpose** (Leave table). Replaced at once. Coming back by the same link reclaims
   the seat.
6. **Coming back after being replaced.** A sync ([§6.7](#67-resynchronising)), and the seat is
   hers again from the next decision. A decision her computer player is already making finishes
   first.

### 5.3 What the others see

- **Name panels show presence:** away, with the grace countdown when it is that player's turn;
  replaced, with a computer badge, and "Computer playing for Ana" in the trick panel; back.
- **Short notices** as toasts: "Ana left the table", "Ana is back".
- **Nothing else changes.** A replaced seat keeps its name, portrait and score. The stand-in's
  moves are the seat's moves.

### 5.4 Solo is different

Nobody else is waiting, so a solo game never replaces you. When you go away, the game holds where
it is, and goes on when you come back, for up to 30 minutes, after which the table closes. There is
no turn timer. A watched game of computer players holds in the same way when its watcher goes away:
it is a table with no human here.

### 5.5 Edge cases

- **An answer and a deadline at the same moment.** The seat driver's lock decides, and the
  generation gate drops the loser. The member is told which won (`promptClosed`).
- **The same token in two tabs.** The newer connection wins; the older one is told why and closed.
- **A human returns while their computer player is deciding.** The decision finishes; the human
  gets the next one.
- **The host goes away in the lobby.** The host role passes on when the grace period ends.
- **Every human away.** The game holds ([§4.9](#49-a-tables-life)).
- **A server restart.** Games in progress are lost until phase 11. Phase 9's drain makes restarts
  rare and announced.
- **Later: a newcomer taking a computer player's seat in a running game.** It is the same
  mechanism: attach a human link to a computer seat at a round boundary. It is not in the first
  version, partly because the newcomer would play under the seat's name (the engine fixes names at
  `start()`).

---

## 6. The protocol

### 6.1 Transport and envelope

- **One WebSocket per tab**, at `/ws`, carrying text frames that each hold one JSON object.
- **Every message has a `type`.** Server messages also carry `seq`, a number that increases per
  connection, for logs and tests.
- **Limits.** Messages from clients are capped at 4 KiB (`setClientMaxWebSocketMessageSize`) and
  rate-limited ([§9](#9-security-and-fairness)). A message over the cap ends the connection with no
  Close frame, so the page sees close code 1006, as for a network drop.
- **No sentences on the wire.** The server sends names, numbers and codes, and each page writes
  the text in its viewer's language. Errors are codes.
- **Words.** The protocol uses the engine's identifiers (`bet`, `betPlaced`); prose and the page
  say "bid", as the desktop client does.

### 6.2 Cards and hands

- **A card is two characters:** the rank `3`–`9`, `T`, `J`, `Q`, `K` or `A`, then the suit `H`,
  `D`, `S` or `C`. `"TH"` is the ten of hearts. The engine has a `Two`, but no deck uses it: the
  deck runs from the ace down, two ranks per player, so six players stop at the 3.
- **A hand is an array in the engine's order.** The page sorts it for display and remembers each
  card's place in the engine's order; that index is what a play sends back. The desktop client
  needed `PlayPrompt::handIndex` because it sorted on the game thread; sorting on the page makes
  that table unnecessary.

### 6.3 Client to server

| `type` | Fields | Allowed |
|---|---|---|
| `hello` | `protocol`, `token` (absent the first time), `lang` | First, and only first |
| `createSolo` | `settings` | Not at a table |
| `createTable` | `settings`, `profile` | Not at a table |
| `joinTable` | `code`, `profile` | Not at a table |
| `updateProfile` | `name`, `gender`, `avatar` | Lobby |
| `takeSeat` | `seat` | Lobby, an open seat |
| `setSeat` | `seat`, `kind` (`open` or `computer`), `level` | Host, lobby |
| `updateSettings` | `settings` | Host, lobby. Solo: speed and the wait at any time |
| `startGame` | – | Host, in the lobby or after a game |
| `bet` | `prompt`, `value` | Your own pending prompt |
| `play` | `prompt`, `index` | Your own pending prompt |
| `continue` | `round` | During the round hold |
| `pause`, `resume` | – | Solo |
| `leaveTable` | – | At a table |

### 6.4 Server to client

| `type` | Fields | To |
|---|---|---|
| `welcome` | `token`, `member`, `server` (`version`, `commit`) | The connection |
| `tableState` | `code`, `mode`, `state`, `host`, `settings`, `seats`, `waitingFor` | Everyone at the table, on every change |
| `game` | `game` (the game's number at the table), `event`, `snapshot` | Everyone, each with their own snapshot |
| `prompt` | [§6.6](#66-prompts-and-answers) | The seat's member |
| `promptClosed` | `id`, `by` (`you` or `computer`) | The seat's member |
| `held` | `round`, `waitingFor`, `deadline` | Everyone, during the round hold |
| `sync` | [§6.7](#67-resynchronising) | On joining or rejoining |
| `error` | `code`, sometimes `detail` | The connection |

**Events** follow the engine's thirteen callbacks one for one: `gameStarted`, `roundStarted`,
`betRequested {seat}`, `betPlaced {seat, bet}`, `bettingComplete`, `trickStarted {trick, leader}`,
`cardRequested {seat}`, `cardPlayed {seat, card}`, `trickWon {seat, trick}`, `roundScored {result}`,
`roundComplete`, `gameOver {standings, winners}`, `gameStopped`. `result` has the desktop client's
`RoundResult` shape. `standings` and `winners` are the engine's `Standing` rows: seat, name, score
and place.

**Computer players** are named as the catalogues name them: `random`, `first-card`, `low-risk`,
`ducking`, `cyborg`, `reckoner-alpha`, `reckoner-beta`, `reckoner-gamma`, `reckoner-delta`.

A `tableState` seat, one of each kind:

```json
{ "kind": "human", "name": "Ana", "gender": "female", "avatar": "woman-long-red-hair",
  "presence": "away", "you": false }
{ "kind": "computer", "level": "cyborg", "name": "T-800", "gender": "male",
  "avatar": "cyborg-man-eye" }
{ "kind": "open" }
```

`presence` is `here`, `away` or `replaced`. `waitingFor` is present while the table waits for an
away player's turn: `{ "seat": 2, "deadline": 23000 }`.

### 6.5 The snapshot

The engine's snapshot ([§3.1](#31-e1-a-snapshot-filtered-for-its-viewer)) for this viewer, as JSON,
plus `you`: the viewer's seat, or `null` for a watcher. A `game` message, six players, mid-trick,
seen from seat 0:

```json
{
  "type": "game", "seq": 812, "game": 1,
  "event": { "kind": "cardPlayed", "seat": 3, "card": "KH" },
  "snapshot": {
    "you": 0,
    "status": "inProgress", "phase": "playing",
    "round": { "index": 7, "count": 32, "type": "normal", "tricks": 3, "trick": 2,
               "trump": "7S", "leader": 2 },
    "active": 3,
    "forbiddenBet": null,
    "bets": { "placed": 6, "sum": 2 },
    "table": { "ledBy": 2, "leadSuit": "H",
               "cards": [ { "seat": 2, "card": "9H", "winning": false },
                          { "seat": 3, "card": "KH", "winning": true } ] },
    "seats": [
      { "name": "Dan", "bidOrder": 5, "bet": 0, "won": 0, "cards": 2, "roundScore": 0,
        "total": 41, "wins": 2, "losses": 0, "hand": ["AS", "TD"] },
      { "name": "Ana", "bidOrder": 6, "bet": 1, "won": 0, "cards": 2, "roundScore": 0,
        "total": 38, "wins": 0, "losses": 1 }
    ]
  }
}
```

The other four seats are cut for space. Only seat 0 has a `hand`. In a Forehead round, the other
seats would have one and seat 0 would not; in a Hidden round, nobody would. `cards` always says
how many backs to draw.

### 6.6 Prompts and answers

```json
{ "type": "prompt", "id": 57, "kind": "play", "deadline": 60000,
  "hand": ["AS", "TD"], "legal": [0], "blind": false }

{ "type": "play", "prompt": 57, "index": 0 }

{ "type": "prompt", "id": 58, "kind": "bet", "deadline": 60000,
  "hand": ["AS", "TD", "4C"], "max": 3, "forbidden": 1, "first": false, "roundType": "normal" }

{ "type": "bet", "prompt": 58, "value": 0 }
```

- **A prompt carries everything its answer needs:** the hand, the legal indices into it, the
  highest bid and the forbidden one. The page never has to consult a snapshot to answer.
- **In a Forehead or Hidden round** the prompt has no `hand`, only `handSize`. This follows the
  desktop client's rule that a prompt which never carries the hand cannot leak it.
- **`deadline` is milliseconds remaining when the prompt was sent**, or `null` when there is no
  timer. The page counts down from when it received it, so the two clocks never need to agree.

### 6.7 Resynchronising

A `hello` with a token that belongs to a live table is answered with a `sync`:

- the `tableState`;
- this viewer's latest snapshot, from the publisher's cache. At every callback the publisher
  caches the latest snapshot for each seat and for a watcher, so answering a sync never touches
  the engine;
- the round history so far, one `RoundResult` per scored round;
- the last finished trick, kept across rounds (the owner's call on the desktop client,
  2026-10-22);
- the pending prompt, if there is one, with the time left on it;
- the round hold, if one is on.

The page drops whatever it had queued and renders the sync.

### 6.8 Errors and versions

- **`hello.protocol` is a whole number.** The page is deployed with the server it was built
  against (§12), so a mismatch can happen only across a deploy. The server answers
  `error {code: "version"}` and the page reloads itself once.
- **Codes:** `badMessage`, `version`, `notFound`, `tableFull`, `nameTaken`, `notHost`,
  `notYourTurn`, `stalePrompt`, `illegal`, `rateLimited`, `draining`. Several `badMessage` errors
  close the connection.

### 6.9 Keeping the two sides in step

- **`protocol/PROTOCOL.md` is the reference.** Each client's types follow it by hand; it is
  small. The page's are in `src/net/protocol.ts`, in `romanian_whist_web`.
- **Contract fixtures.** Server tests write one real sample of every message to
  `protocol/fixtures/`, taken from real games. Each client's tests decode every fixture with that
  client's own decoder, through its submodule of this repository. A field renamed here fails a
  client's tests once that client's submodule moves to the commit that renamed it.

---

## 7. Tables, seats and players

### 7.1 Solo

The desktop client's New Game, as a page:

- play or watch;
- the number of seats, and a computer player per seat (the nine);
- names, drawn from the catalogue and editable;
- the game's shape, and the Forehead and Hidden options;
- bot speed and "Wait for me after each round";
- a fixed deal (a seed).

Start goes straight to the table.

### 7.2 Online tables

- **Creating** takes the solo settings, without the seed and without watching, plus two more:
  - **the replacement computer player.** The default is Reckoner Beta: it has the best result in
    the engine's tournament tables (87.5 points a game against the Cyborg's 72.7, over 15 games)
    and costs less than Gamma or Delta;
  - **the turn timer:** 60 s by default, or off.
- **The creator is the host**, and gets a link, `https://<host>/t/K7QM4P`, and its code.
- **The lobby** shows the seats round the table. Each seat is open, a human (with presence), or a
  computer player (with its level). People who join take an open seat. The host can make a seat a
  computer player and choose its level, open it again, move people, change the settings, and
  change the number of seats (2 to 6).
- **Starting** needs every seat filled. The start button offers to give the open seats to computer
  players.

### 7.3 Names, genders and portraits

- **Humans** choose:
  - a name of 1 to 20 characters, the desktop client's limit;
  - a gender, which Romanian needs ("Ana e prima", "Bogdan e primul");
  - a portrait from the catalogue's entries for that gender, or for either, that are not dedicated
    to a strategy.
- **Names are unique at a table, compared without regard to case.** The engine requires names
  that differ byte for byte, and "Ana" next to "ana" would confuse everyone anyway. A clash gets a
  number ("Ana 2").
- **Cleaning.** Trim, normalise to NFC, and strip control characters and bidirectional overrides,
  which can make one name display as another.
- **Avoid "LowRisk" and "Ducking" in names.** `ReckonerStrategy::onGameStarted()` models a seat
  whose name contains either word as deterministic, so a human so named would be misread by every
  Reckoner at the table.
- **Computer players** get names and portraits by the desktop client's rules, from
  `player_names.json` and `avatars.json`: entries dedicated to the strategy first (the Cyborgs'
  names and faces), in the host's language.

### 7.4 Who may change what, and when

| Setting | Solo | Online |
|---|---|---|
| Seats, their kinds and levels; the game's shape; Forehead and Hidden | Before the start | Host, in the lobby |
| Bot speed | At any time | Host, in the lobby |
| "Wait for me after each round" / the round hold | At any time | Host, in the lobby |
| Seed | Before the start | Not offered |
| Replacement computer player, turn timer | Not used | Host, in the lobby |
| Animation speed, language | Each viewer, at any time | Each viewer, at any time |

---

## 8. The browser page

The page has its own plan, in `romanian_whist_web`:
[`docs/page-plan.md`](https://github.com/conectionist/romanian_whist_web/blob/master/docs/page-plan.md).
This section's parts are that plan's sections:

| Here | In the page's plan |
|---|---|
| 8.1 The look: the desktop client wins | [§2](https://github.com/conectionist/romanian_whist_web/blob/master/docs/page-plan.md#2-the-look-the-desktop-client-wins) |
| 8.2 Technology | [§3](https://github.com/conectionist/romanian_whist_web/blob/master/docs/page-plan.md#3-technology) |
| 8.3 Code layout | [§4](https://github.com/conectionist/romanian_whist_web/blob/master/docs/page-plan.md#4-code-layout) |
| 8.4 The playback queue | [§5](https://github.com/conectionist/romanian_whist_web/blob/master/docs/page-plan.md#5-the-playback-queue) |
| 8.5 Rendering | [§6](https://github.com/conectionist/romanian_whist_web/blob/master/docs/page-plan.md#6-rendering) |
| 8.6 From the mock-up's script | [§7](https://github.com/conectionist/romanian_whist_web/blob/master/docs/page-plan.md#7-from-the-mock-ups-script) |
| 8.7 From the desktop client | [§8](https://github.com/conectionist/romanian_whist_web/blob/master/docs/page-plan.md#8-from-the-desktop-client) |
| 8.8 Assets | [§9](https://github.com/conectionist/romanian_whist_web/blob/master/docs/page-plan.md#9-assets) |
| 8.9 Languages | [§10](https://github.com/conectionist/romanian_whist_web/blob/master/docs/page-plan.md#10-languages) |
| 8.10 Keyboard, screen readers, touch and small screens | [§11](https://github.com/conectionist/romanian_whist_web/blob/master/docs/page-plan.md#11-keyboard-screen-readers-touch-and-small-screens) |

The page's tests, its phases (6, 7, 8 and 10), its risks and its decisions (D5, D14 and D18) are
there too.

---

## 9. Security and fairness

**Hidden information** (rules 1 and 2):

- The engine's viewer-filtered snapshot is the only thing that reads hands for the network.
  `grep -rn "getHand()" server/` should find nothing: the server sees hands only through the
  snapshot and the move contexts.
- A watcher never sees hands while humans play.
- Seeds never reach clients online, because a seed is the whole deal; solo players may set their
  own.
- The forbidden bid is public, since anyone can work it out from the bids.
- Stand-ins decide from their seat's own hand and the public play, as the engine's computer players
  always do.

**Untrusted input:**

- **Size and rate.** Messages are capped at 4 KiB. Each connection may send about 20 messages a
  second, with bursts to 40; beyond that it gets an error, then is closed. HTTP request bodies are
  capped at 16 KiB, Drogon's in-memory limit, so Drogon never writes one to disk.
- **Strict decoding.** Unknown types and unknown fields are rejected.
- **Every answer is checked** by its seat driver ([§4.7](#47-a-seats-driver)).

**Identity:**

- **The token** is 128 random bits (base64url), kept in the browser's storage and sent only in
  `hello`, never in a URL or a log.
- **One live connection per token.**
- **A join code** is six characters from an alphabet without look-alikes (about 30 bits), so
  failed joins are rate-limited per IP address, and tables expire.

**Limits:** connections per IP address (`setMaxConnectionNumPerIP`), tables created per IP address
per hour, a maximum number of running games, and members per table no more than its seats.

**The page:**

- The server sends it with a strict Content-Security-Policy: no inline script, and
  `connect-src 'self'`.
- The page's own rules, such as player-chosen text going in only through `textContent`, are in
  [its plan's §12](https://github.com/conectionist/romanian_whist_web/blob/master/docs/page-plan.md#12-security).

**Fairness:**

- A timed-out move is made by the seat's computer player, never by a random card.
- The turn timer is long enough to cover the page's animations, and the page shows it.
- A stand-in plays to the seat's own bid, with the seat's own hand.

**Licence:** the engine is AGPL-3.0-or-later. A game server built on it must offer its users the
source of what is running (the licence's section 13). The page's footer links to the repository at
the exact commit the server was built from, served by `GET /version`
([§12](#12-deployment-and-operations)).

---

## 10. Testing

### 10.1 The engine (phase 0)

- **E1:** the pinned snapshots and the leak property
  ([§3.1](#31-e1-a-snapshot-filtered-for-its-viewer)).
- **E2:** the stand-in takeovers
  ([§3.2](#32-e2-strategies-standing-in-for-a-seat-they-do-not-play)).

### 10.2 The server's core

These are Catch2 tests against `server/core`, with no sockets. They use the in-memory `Outbox`,
timings set in milliseconds through configuration, and seeded games.

- A solo game played to the end by a scripted human; every message decodes.
- Invalid, stale and doubled answers are refused and re-prompted, and the game goes on.
- **The JSON leak scan.** For every message to every member, every card in it is one that member
  may see: their own hand when `canSeeHand()` allows, the cards on the table, the cards already
  played, the trump.
- Each timeline of [§5.2](#52-timelines), with short timings.
- An answer and a deadline at the same moment, repeated many times.
- Closing a table while a human is parked, while pacing, during the round hold, and while a
  Reckoner thinks. The thread joins within a second each time.
- Every human away: the game holds; one returns: it goes on; nobody returns: the table closes.
- A sync carries what a fresh viewer would get.
- A fixture is written for every message type ([§6.9](#69-keeping-the-two-sides-in-step)).

There is a `tsan` preset. Run it once at the end of each server phase, with the owner's go-ahead
([§11](#11-building-and-running-locally)).

### 10.3 The protocol

The contract fixtures of [§6.9](#69-keeping-the-two-sides-in-step).

### 10.4 End to end

- **The page's browser tests**, of a solo game, an online game, a reload and a drop, are in
  [its plan's §13.2](https://github.com/conectionist/romanian_whist_web/blob/master/docs/page-plan.md#132-end-to-end).
- **A chaos test:** a Node script with a dozen headless protocol clients that disconnect and
  reconnect at random, run in the background at low priority. It passes when every game finishes,
  no table stalls, and the leak scan, run again on the client side, finds nothing.

### 10.5 The look

The page's comparisons with the desktop client are in
[its plan's §13.3](https://github.com/conectionist/romanian_whist_web/blob/master/docs/page-plan.md#133-the-look).
The table's fixtures come from a tool in this repository, in `tools/`, built in phase 6. It plays
the desktop client's `tests/ScreenshotTool.cpp` games, with the same seeds and players, to the same
moments, and writes each viewer's snapshot into `protocol/look/`. The snapshots are committed, and
the page reads them through its submodule. They are snapshots rather than whole messages, so the
decoding tests of §6.9 read `protocol/fixtures/` only. The sheets' fixtures are built by hand, in
the page's repository.

### 10.6 Load (phase 9)

- **Tables of six Reckoner Gammas**, at normal speed and at Instant.
- **Measured:** CPU per table, the time from a prompt being sent to it arriving, and memory.
- **The result sets the maximum number of running games.**

---

## 11. Building and running locally

**The owner's machine rules apply unchanged.** The section "Never freeze the owner's computer" in
the desktop repository's `CLAUDE.md` goes into this repository's `CLAUDE.md`:

- builds as `nice -n 19 cmake --build <dir> -j 2`;
- tests under `nice -n 19`;
- one heavy command at a time, and long jobs in the background;
- timing runs, sanitizer builds, the chaos test and the load test only when the owner asks.

The first build of each preset compiles Drogon too, which takes several minutes at `-j 2`, so run
it in the background.

**The owner's machine**, as checked on 2026-10-27, is Linux Mint 22.3, which is built on Ubuntu
24.04.

| Present | Missing |
|---|---|
| g++ 13.3, clang 18 | Emscripten (phase 10 only) |
| CMake 3.28 | Caddy (deployment only) |
| Node 24, npm 11 | |

Drogon's system packages are listed in [§4.2](#42-why-drogon-and-what-it-does-not-do).

- **Presets:** `default` (Debug), `release`, `clang` and `asan`, as in the desktop client, plus
  `tsan`.
- **Day to day:** the server serves `server/www/` by default, or the page's built `dist/` when
  `document_root` points there, as in production. The page's own dev server is in
  [its plan's §14](https://github.com/conectionist/romanian_whist_web/blob/master/docs/page-plan.md#14-building-and-running-locally).
- **CI:** one Linux job, which builds the server with a fixed job count (as the desktop client's CI
  does, never an unlimited `-j`) and runs its tests. The page has a CI job of its own.
- **Pull requests:** change a PR's title or description with
  `gh api -X PATCH repos/<owner>/<repo>/pulls/<N> -F body=@<file>`. `gh pr edit` fails on the
  owner's repositories with a "Projects (classic)" GraphQL error.

---

## 12. Deployment and operations

- **A small Linux server:** 2 vCPUs and 2–4 GB, Debian or Ubuntu LTS. The server runs as a release
  build, with the page's built `dist/` as Drogon's document root, and one configuration file. The
  page is built from the latest `romanian_whist_web` commit whose submodule points at the server
  being deployed. A server change that no such commit points at gets a submodule update in
  `romanian_whist_web` before it is deployed. So the two speak the same protocol, except in a page
  left open across a deploy (§6.8).
- **Caddy in front** gives HTTPS automatically. `reverse_proxy localhost:8080` passes WebSockets
  through as they are. Drogon listens on localhost only.
- **A systemd unit** restarts the server if it fails and raises its file limit. Logs go to the
  journal.
- **A drain on SIGTERM.** The server installs its own SIGTERM and SIGINT handlers, which only set a
  flag, and turns Drogon's off (`disableSigtermHandling()`). Drogon's handlers write a log line
  inside the signal handler, which can allocate memory, and so hang. A timer on the main loop sees
  the flag and drains:
  1. refuse new tables;
  2. tell everyone the server is restarting;
  3. wait for running games to end, or for at most 15 minutes, then abandon and join any still
     running ([§4.6](#46-threads-and-locks), rule 7);
  4. quit.

  Deploys wait for a quiet moment.
- **Health:**
  - `GET /health` (`app().registerHandler`), for uptime monitoring;
  - one log line a minute with counters: connections, tables by state, game threads, moves made by
    stand-ins, CPU.
- **The licence:** `GET /version` gives the commit, and the page's footer links "Source" to this
  repository at that commit, which covers the AGPL's section 13 for the server and the engine. It
  links `romanian_whist_web` too, at the commit the page was built from.
- **Privacy:** no accounts and no cookies, only a chosen display name and a random token in the
  browser's storage. An About page says so.

---

## 13. Phases

Each phase is a branch and a pull request. Its outcome is written under it when it closes: what was
built, what the owner decided, and where the build departed from this plan. Sizes are relative: S,
M, L.

**Branches are stacked**, as the owner set out on 2026-11-02:

- **The parent branch** comes off master, in each repository a phase changes.
- **Each phase gets its own branch** off that parent, `web/phase-N-<name>`.
- **Before the merge:** the phase branch's CI, a code review and the owner's review all pass, and
  the phase is marked complete in this plan, with its outcome.
- **Then** the phase branch is merged into its parent.
- **When every phase is done,** the parent branch is merged into master.

**Code reviews**, as the owner set them out on 2026-11-02:

1. **The reviewer starts cold.** It is a fresh agent with none of the implementer's context (never
   a fork of the implementer), runs at extra-high effort, and is read-only. Its prompt holds only:
   - the repository, the phase branch and its parent;
   - the phase's requirements, as they stand on the parent branch;
   - the repository's `CLAUDE.md`;
   - the rating scales.
2. **Each finding comes back rated 1 to 5:**
   - **severity:** from cosmetic (1), through an edge case (3), to a crash, a hang, a leaked card
     or lost data (5);
   - **fix complexity:** from a line or two (1), through a new test (3), to a redesign (5).
3. **The implementer reports them** in a table, checks each one against the code, and then gives
   their own view: what to fix, in what order, and what to skip.
4. **The owner decides.** Nothing is fixed before that.
5. **After the fixes**, both presets are built and tested and CI runs again. The owner decides
   whether the fix commit gets a cold review of its own.
6. **A phase's outcome is written after its review**, and the checkouts sit on their parent
   branches while a reviewer reads, so a reviewer never sees the implementer's account.

After phase 3, the page (phases 6–8, in its own plan) can be built against fixtures while phases 4
and 5 go on in the server. Phase 6 starts in the server, with the fixture tool (§10.5).

### Phase 0: engine groundwork (the engine repository) — M ✅ COMPLETE

- **Work:** E1 and E2 (§3.1, §3.2); E3 if phase 10 is wanted. README and CHANGELOG. Release 5.1.0.
- **Done when:** the pinned snapshots, the leak property and the stand-in tests pass on CI on all
  three platforms.

**Outcome** (engine branch `web/phase-0-engine-groundwork`, commit `fc4b8ec`, merged into the
engine's parent branch, then into master as `ff223ef`, which is tagged `v5.1.0`):

- **E1, the snapshot.** It lives in `romanian_whist/Snapshot.h`: `GameSnapshot`, `SeatSnapshot` and
  `snapshotFrom(engine, viewer)`, with §3.1's fields. Where the build departs from §3.1:
  - **`winning` is one seat**, `std::optional<Seat>`, not a flag on every card. The table reuses
    the engine's own `PlayedCard`. The server can still write a flag per card in §6.5's JSON.
  - **Seats are `std::optional<Seat>`**, not plain indices. `roundLeader` and `ledBy` are engaged
    in every snapshot.
  - **The snapshot carries its `viewer`**, which is where §6.5's `you` comes from.
  - **It refuses bad calls:** `std::logic_error` before `start()`, and `std::out_of_range` for a
    viewer who is not at the table.
  - **The header guard is `ROMANIAN_WHIST_SNAPSHOT_H`**, because the desktop's own
    `src/core/Snapshot.h` already uses `SNAPSHOT_H`. Should the desktop client adopt the engine's
    snapshot, its global `GameSnapshot` would clash with `romanian_whist::GameSnapshot` in the files
    that say `using namespace romanian_whist;`.
- **E1's tests,** in `tests/SnapshotTests.cpp`:
  - **Eight pinned frames**, as text. Besides §3.1's points, they pin a Forehead deal and
    `onGameOver`.
  - **The leak property** covers 80 games: 4 seeds at each table size, both structures, and both
    Forehead settings. It checks every viewer's snapshot at every callback, against `canSeeHand()`
    and against a truth table written in the test. 200 games took 11 s in the Debug build, so the
    count was cut.
  - **A second leak property stops 50 games**, some at a random callback and some during a
    Forehead round's bidding, so `onGameStopped()` is checked too. Closing a table does this.
  - **`ledBy` is followed across every frame of a trick.**
  - **Mutation checks** confirmed that the leak property catches a missing `canSeeHand()`, and the
    drift test catches `ledBy` read from `getTrickLeaderSeat()`.
- **E2, the stand-ins.** A README section, "A strategy for a seat it does not play", and the
  promise on `ReckonerStrategy::setSeat()`. `tests/StandbyTests.cpp` tests the Cyborg and all four
  Reckoner presets at seven moments: a bid, a round's first lead, a lead mid-round, a follow, a
  Forehead bid, a Hidden bid, and any decision. Where the build departs from §3.2:
  - **The memory check.** The Reckoner has no fallback counter: it quietly plays its first legal
    card when its memory disagrees. So the test checks the stand-in's memory against the engine at
    every one of the seat's decisions, before and after the takeover, for the Cyborg too.
  - **Added: an equivalence test.** A stand-in that takes over at the first decision plays exactly
    the game of the same strategy seated by its factory. This is checked for the Cyborg and Reckoner
    Alpha at every table size, and for Reckoner Beta at 3.
  - **Gamma and Delta play at 2 seats**, to keep CI's Debug cell quick. Games alternate 1-8-1 and
    8-1-8, and "any decision" can fall late in a game.
  - **Added: a shared seat.** The stand-in makes one decision in three, and the seat's own player
    the rest, as when moves are made for an idle player (§5.2, timeline 4). After each of the
    player's leads, the test checks the Cyborg's cashing run exactly.
  - **Fixed in the Cyborg.** It remembered the suit it was cashing, and only its own leads changed
    that. Sharing a seat, it never saw its player's leads, so it kept cashing a suit the seat had
    moved on from. Now a lead it did not choose ends the run when it is in another suit, and carries
    the run on when it is in the same suit. A Cyborg playing its own seat plays exactly as before.
  - **Found:** the Reckoner treats any seat whose name contains "LowRisk" or "Ducking" as
    deterministic, `setSeat()` or not. §7.3 already keeps humans off those names, and the README
    now says so.
- **Cost.** The new tests take about 2.6 s in Release and 16 s in Debug, where the whole suite took
  44 s before.
- **Reviews.**
  - **A first code review**, before the procedure above existed, ran at high effort from an agent
    that could see my context. It found 10 small problems; 7 were fixed.
  - **The first cold review**, of the whole branch, found 7 problems, none above severity 2. The
    owner chose my recommendation:
    - fix the stale cashing suit, test stopped games, and correct the wording;
    - add late takeovers and both structures to the stand-in tests;
    - leave the name clash with the desktop's `GameSnapshot` for the desktop to solve, if it ever
      adopts the engine's.
  - **That review's tool read this plan's working copy, outcome note and all.** Hence the
    procedure's last point.
  - **The second cold review**, of the fix commit, found 8 problems, none above severity 2. The
    worst was that the fix ended the run even when the player carried on in the same suit. All 8
    were fixed, as the owner chose.
  - **CI** ran by hand on the branch (run 166, at `fc4b8ec`) and is green on Linux, Linux Debug,
    Windows and macOS.
  - **The owner approved** the phase on 2026-11-02, and chose to skip a third cold review of the
    last two fix commits.
- **Left out, by the owner's choice:** E3 (the Emscripten CI job) and the Reckoner refusing a name
  that matches no seat.

### Phase 1: skeleton — S ✅ COMPLETE

- **Work:**
  - this repository's `CLAUDE.md`, both submodules, the presets, Catch2, and a Vite project (the
    repository and its licence already exist);
  - CI;
  - Drogon serving a static page and a WebSocket echo at `/ws`.
- **Spikes to settle what this plan assumes about Drogon:**
  - a send from a non-I/O thread;
  - pongs reaching `handleNewMessage`;
  - the client message size cap;
  - the SIGTERM handler.
- **Done when:** it builds and tests under the machine rules, the page loads from the server, the
  echo works, and the four spikes behave as [§4.2](#42-why-drogon-and-what-it-does-not-do) and
  [§5.1](#51-words) assume. Where they do not, this plan is amended.

**Outcome** (branch `web/phase-1-skeleton`, PR #2, commit `8be1a50`):

- **Built.**
  - **Submodules:** the engine at `v5.1.0` (`ff223ef`), and Drogon at `v1.9.14` (`3aae769`)
    with trantor (`78e480f`). Both are built with the project. Drogon's ORM, `drogon_ctl`, examples,
    Brotli, YAML and c-ares are off.
  - **Presets:** `default`, `release`, `clang`, `asan` (UBSan stops at its first error) and `tsan`.
  - **`server/core`** (`whist_session`): links the engine and never Drogon. Its test reads the
    engine's viewer-filtered snapshot through the submodule.
  - **`server/app`:** `whist_server`, made of three parts:
    - `ServerOptions`, read from one JSON file, which refuses unknown keys, port 0 and host
      names;
    - `configureApp()`, which `main()` and the tests share;
    - `EchoController`, the echo at `/ws`.

    Drogon serves `web/dist`.
  - **`server/tests`:** `core_tests` and `app_tests`, 12 tests in all. They cover the page, the
    echo, the settings, a request body over the cap, and the four spikes.
  - **`web/`:** an echo page, built with Vite 8.3.4 and TypeScript 7.0.2, with no framework. It
    has a Vitest 5.0.5 unit test and a Playwright 1.63.0 test that runs the page against the real
    server.
  - **CI:** one Linux job. It builds the release preset with warnings as errors, then runs the
    server tests, the page build and the page's unit tests.
  - **`CLAUDE.md`** with the machine rules, the comment rule, the branch workflow and the PR note,
    and a README.
- **The spikes.**
  - **A send from a thread that is not Drogon's works.** Two threads of ours sent 500 numbered
    messages each, at once. Every message arrived whole, and each thread's arrived in order.
    Sending after the client had gone did no harm. The reviews found three limits, now in §4.2,
    §4.4 and §4.6 (rule 7):
    - order is not kept against sends made on the connection's own I/O thread;
    - nothing but `send()` may be called from another thread, and a `disconnected()` call raced;
    - a send after `app().quit()` reaches freed memory.
  - **Pongs reach `handleNewMessage`,** as §5.1 assumed.
  - **The size cap holds.** A message of 4096 bytes is echoed. One of 4097 bytes ends the TCP
    connection with no Close frame, so a browser sees close code 1006 (§6.1).
  - **SIGTERM: Drogon's handler can't be used.** Drogon logs inside the signal handler, then calls
    the handler set with `setTermSignalHandler()` there too. So the server turns Drogon's handling
    off and installs its own SIGTERM and SIGINT handlers, which only set a flag. A timer on the main
    loop then serves for `shutdown_delay_ms` more and quits. The test runs the real server: after
    SIGTERM the echo still answers, and the process exits 0 (§12).
- **Where the build departs from the plan, or found what it did not say.**
  - **The settings** are the server's own JSON file, not Drogon's configuration file, so the tests
    set up the server exactly as `main()` does.
  - **Port 0 is refused.** Drogon opens one listening socket per I/O thread, and with port 0 each
    socket gets a different port. The tests pick a free port.
  - **"Listening on"** is logged only once every I/O loop has run its `listen()`. Drogon queues
    `listen()` on each loop just after the beginning advices.
  - **Host names are refused,** because Drogon aborts on one.
  - **The upload folder.** Drogon makes one at start, with 256 folders inside it, and writes there
    any request body over its in-memory limit. So the server caps request bodies at 16 KiB, which is
    also that limit, and puts the folder, one per user, in the temporary directory (§9).
  - **Drogon's WebSocket client** reconnects by itself a second after a drop, and calls its connect
    callback again. Only the tests use it, and they allow for that.
  - **CMake.** Trantor's old policies needed `CMAKE_POLICY_DEFAULT_CMP0077`, and a cache entry for
    `TRANTOR_USE_TLS`. `add_subdirectory(... SYSTEM)` needs CMake 3.25.
  - **npm 10 fails to install these packages,** with an `edgesOut` error, so `package-lock.json` was
    made with npm 11.
  - **Left as it is:** a test picks a free port and closes it before the server binds it, so another
    process could take it first. Drogon would then end the test process. That is unlikely with
    tests run one at a time.
- **Reviews.**
  - **The first cold review**, of the whole branch, found 13 problems, none above severity 3. The
    owner chose my recommendation: fix 1 and 4 to 13, and record 2 (send order) and 3 (sends after
    `quit()`) in this plan.
  - **A second `tsan` run** found a data race that one of those fixes had added, in the
    send-after-close spike.
  - **The second cold review**, of the fixes, found 5 problems, none above severity 3, with that
    race first. The owner chose my recommendation: fix 1 to 4, note 5 here, and skip a third
    review.
  - **`tsan`,** three runs, each with the owner's leave: clean at `2e8b420`, one race at
    `0b5704a`, and clean at `8be1a50`.
  - **The default and release presets** pass 12 of 12 at `8be1a50`, and the browser test passes.
    CI is green on runs 1, 2 and 4, at `2e8b420`, `0b5704a` and `362be11`. Run 3, at `8be1a50`,
    was cancelled when run 4 started, and run 4 has the same code.
  - **The owner approved** the phase on 2026-11-03.

### Phase 2: the server's core — L ✅ COMPLETE

- **Work:** `Registry`, `Table`, `GameRun`, `SeatDriver`, `Publisher` and `Outbox` in
  `server/core`; the JSON encoding of every message; the fixtures.
- **Done when:**
  - a solo game runs headless to the end with a scripted human;
  - bad answers re-prompt;
  - abandonment joins the thread;
  - the JSON leak scan passes;
  - the core tests pass, and one `tsan` run does too.

**Outcome** (branch `web/phase-2-server-core`, PR #5, commit `dd12b44`):

- **The owner's choices before it was built** (2026-11-03):
  - the seat driver's deadline path is built now, rather than in phase 5;
  - fixtures cover every message type now, two of them from fixed values until a game sends them;
  - every prompt ends with exactly one `promptClosed`.
- **Built,** in `server/core`, which still links no Drogon:
  - **The codec.** Strict decoding of every client message: unknown types and fields refused,
    whole numbers only, UTF-8 checked, nesting capped at 16. An encoder for every server message.
    Cards are written `"TH"`; the engine's `Card::toString()` writes the ten as `"10"`.
  - **Settings,** with every check the engine's `start()` would otherwise fail on the game thread,
    and the nine computer players. A Cyborg or Reckoner is given its seat with `setSeat()`.
  - **`Outbox`,** over a `Connection` seam that only queues.
  - **`SeatDriver`,** a port of the desktop's `QtMoveProvider`.
  - **`Publisher`,** with the desktop's pacing, an Instant floor of 30 ms, and the cache a sync is
    built from.
  - **`GameRun`, `Joiner`, `Table` and `Registry`:** solo tables, tokens and codes, a new game at
    the same table, closing, and shutdown.
  - **`protocol/PROTOCOL.md`**, and 24 fixtures in `protocol/fixtures/`. The core's tests check
    them on every run, and `WHIST_WRITE_FIXTURES=1` rewrites them.
  - **One wait helper,** `core/Wait.h`, through which both game-thread waits go: §4.1's seam for
    phase 10.
- **Tests:** 38 new core tests, 39 in all, and a CTest that no server code calls `getHand()`.
  - **The leak scan.** An observer registered before every other one records which cards each
    member may see, round by round. Every card in every message is checked against it, syncs,
    history and last trick included, over 20 games at 2 to 6 seats with a watcher.
  - **Mutation checks.** A blind prompt carrying its hand, and a viewer sent another seat's
    snapshot, each fail the scan.
  - **Cost.** The core tests take about 30 s in Debug, 24 s of it the leak scan, and about 25 s in
    Release.
- **Where the build departs from the plan, or found what it did not say:**
  - **The `Outbox` keeps the member-to-connection map** (§4.4 put the connection on the `Member`).
    The game thread deals in member ids and never touches a `Member`.
  - **Sending under a lock.** The publisher sends its game messages under its cache lock, and the
    table sends under its own. That keeps a sync in order with the live messages. It is safe
    because `Outbox::send` only queues and its locks are leaves, but it departs from §4.6 rule 4's
    "no lock is held while sending".
  - **Who checks an answer.** The I/O thread only gates it (an open prompt, the right id, not
    answered yet). The game thread checks it and re-prompts. So every message about a prompt comes
    from the game thread, in order.
  - **Prompt ids are counted per table** and never reused, so an answer from an earlier game cannot
    match a new prompt.
  - **The deadline is checked even when an answer wakes the wait.** Otherwise a stream of invalid
    answers would hold the turn timer off for ever.
  - **The `settings` shape,** which §6 never gave, is defined in `PROTOCOL.md`.
  - **A round result** has the desktop's fields with a 0-based `index`, without the names or the
    `held` flag.
  - **No callback into the table.** A game never calls back into its table; the table reads
    whether its game is over. A game that is over stays in the table until the next game or the
    close.
  - **A solo human seat has no computer player behind it,** because solo never replaces (§5.4).
  - **Added: `RunOutcome::Failed`.** A game that fails ends with `gameStopped`, built from the cache.
- **Left for later phases:**
  - **Phase 3:** what `waitAfterRound`, `pause`, `resume` and `continue` do. They are decoded now.
  - **Phase 4:** the name cleaning of §7.3: trimming, NFC, and refusing invisible characters. Names
    are only checked now.
  - **Phase 5:** the reaper.
- **Reviews.**
  - **The cold review,** of the whole branch, found 9 problems, none above severity 2: no race,
    deadlock or leaked card. The worst was a sync that could carry a round's result before its
    snapshot reached that round. The owner chose my recommendation:
    - fix 1 to 4, 6, 8 and 9;
    - leave 5 (name cleaning) to phase 4, and 7 (the wait after each round, the reaper) to
      phases 3 and 5.
  - **The owner chose to skip a second cold review** of the fixes.
- **Builds and CI.**
  - **`default` and `release`** pass 51 of 51 at `dd12b44`.
  - **`tsan`,** one run with the owner's leave, was clean at `abbead5`. The fixes after it add no
    lock and change no lock order.
  - **CI** is green on runs 10 and 12, at `abbead5` and `b6a7cef`. Run 11, at `dd12b44`, was
    cancelled when run 12 started, and run 12 has the same code.
  - **The owner approved** the phase on 2026-11-03.

### Phase 3: solo, end to end — M ✅ COMPLETE

- **Work:**
  - the Drogon controller over the core: `hello`, tokens, `createSolo`, prompts, answers, `sync`;
  - pause, and settings changed mid-game;
  - a plain debug page in `server/www/`, in place of the placeholder: the snapshot as text,
    buttons for the legal bids and cards.
- **Done when:**
  - a whole solo game can be played in a browser;
  - a reload mid-trick resumes it;
  - a watched game of computer players runs to the end.

**Outcome** (branch `web/phase-3-solo`, PR #6, commit `b36a045`):

- **The owner's choices** (2026-11-03 and 04):
  - the branch name;
  - the browser check is done by hand, in headless Chromium with screenshots, rather than by a
    Playwright test in this repository, which has no Node since the page moved out;
  - after the review: fix 1 and 3 to 7, leave 2 to phases 5 and 9, no second cold review, and one
    `tsan` run.
- **Built.**
  - **In `server/core`, still with no Drogon:**
    - **A `Session` per connection,** which takes `hello` first and once, finds or makes the member
      behind a token, and handles `createSolo`, answers, pause and resume, `updateSettings`,
      `continue`, `startGame` and leaving a solo table.
    - **A `Service`,** which owns the `Outbox` and the `Registry` and shuts both down: every game
      joined, then nothing more sent.
    - **The `Outbox` numbers each connection on its own,** closes connections, and stops for good at
      shutdown.
    - **Pause and "wait after each round" share one hold** in the publisher's rest, as in the
      desktop's `pace()`. A pause lands after a computer's bid, before its card, or on a finished
      trick or round. A new speed applies to the beat under way.
  - **In `server/app`:**
    - **`GameController` at `/ws`** replaces the echo. Its `WsConnection` holds Drogon's connection
      weakly and always queues its sends on the connection's own loop.
    - **`stopApp()`** joins every game thread before `app().quit()` (§4.6, rule 7). The SIGTERM
      timer and the tests both stop through it.
    - **`instant_floor_ms`** sets the shortest computer beat.
  - **`server/www/`:** a plain debug page. Presets fill an editable settings box; the page shows
    the snapshot as text, with a button for each legal bid and card, pause, speed, the round wait,
    Continue, Play again and Leave.
  - **`PROTOCOL.md`** covers all of this. The `welcome` and `held` fixtures come from a real game
    now.
- **Tests:** 21 new core tests, 60 in all, and 5 new app tests in place of the echo's, 15 in all.
  CTest runs 76.
  - **Core:** the session's rules; two tabs racing for one token; fifty reconnects during a game,
    each starting with its `welcome` and with nothing after its close; a whole solo game through a
    session, and Play again at once; a reload mid-trick; a watched game, its watcher reloading;
    pause; the round wait; live speed and settings; leaving mid-game. Closing a game while paused
    and during a hold joins within a second. The leak scan goes through holds.
  - **App,** over the real socket: `hello`, a watched game, a solo game, a reload mid-trick, a
    second tab taking over, and SIGTERM with two games running.
  - **The browser,** by hand: the debug page played a 3-seat 8-1-8 game to the end, through 23 holds
    and a pause, with no page errors. A reload mid-trick brought back the same snapshot, prompt and
    last trick, and a watched game ran to its end.
- **Where the build departs from the plan, or found what it did not say:**
  - **`openElsewhere`,** a new error code. A second tab with the same token takes the member over,
    and the older is told why and closed (§5.5). A page that gets it should not reconnect by
    itself.
  - **`seq` counts per connection,** from before `hello`, and the `welcome` is always first.
  - **A `sync` follows every `welcome`,** with `table: null` at no table, so a page knows to wait.
  - **`tableState.paused`,** and **`held.deadline: null`** in solo, which waits as long as it
    takes. In a watched game `held.waitingFor` is empty, and the watcher continues.
  - **Errors go back on the connection that caused them.** The one exception is the game's
    `illegal` for an invalid answer, which goes to the member's connection at that moment.
  - **A table silences a game it lets go of,** so a closed table's `tableState` is its last message,
    and leaving mid-game then starting again cannot mix two games' messages.
  - **A table reads as finished once `gameOver` is out,** so Play again right after it is not
    refused. No `tableState` is sent for the game's end; the `gameOver` event is it.
  - **The desktop's pause, ported as it is:** answers are taken while paused, and the game holds at
    its next rest. Turning the wait off leaves a hold already on.
  - **Solo `leaveTable` is built here,** though §13 puts Leave table in phase 5. Without it, a
    player could not start a game with other settings.
  - **The fifth `badMessage` closes a connection** (§6.8's "several"), and **"already at a table"
    is `illegal`,** not `badMessage`.
  - **The commit in `welcome`** is recorded when CMake configures, so it can lag behind. Phase 9's
    `/version` makes it exact.
- **Left for later phases:**
  - **Phases 5 and 9: nothing yet removes abandoned solo games or caps them.** A member is never
    forgotten, a solo game left on a prompt or a pause keeps its thread for good, and there is no
    limit on connections or running games. The cold review rated this severity 2. **The server
    must not be public before the reaper and the limits are built.**
  - **Phase 5:** the hold while a solo player is away, and its 30 minutes.
  - **Phase 6:** the page's echo test changes when its submodule moves here.
- **Reviews.**
  - **The cold review,** of the whole branch, found 7 problems, none above severity 2, and no race,
    deadlock or leaked card. The worst was that the game-over flag was set just after `gameOver`
    was sent, so a Play again in that moment could be refused. The owner chose my recommendation:
    - fix 1 and 3 to 7;
    - leave 2 (no reaper, no limits) to phases 5 and 9.
  - **The owner chose to skip a second cold review** of the fixes.
- **Builds and CI.**
  - **`default` and `release`** (warnings as errors) pass 76 of 76 at `b36a045`.
  - **`tsan`,** one run with the owner's leave, passes 76 of 76 at `b36a045`. A race report would
    have failed its test, the server spawned by the SIGTERM test included.
  - **CI** is green at `ec17cc3` and at `b36a045`.
  - **The owner approved** the phase on 2026-11-04.

### Phase 4: online tables — M

- **Work:** create, join, the lobby, seats, profiles, host controls, start; the round hold;
  `tableState`; names and portraits by the catalogues' rules.
- **Done when:** three browsers and three computer players finish a game together.

**Outcome** (branch `web/phase-4-online-tables`, PR #7, commit `ccdfd14`):

- **The owner's choices** (2026-11-04):
  - the branch name;
  - utf8proc for the Unicode data names need;
  - a finished table is a lobby again: seats, settings, profiles, joining and leaving all work
    until the next game, and its `state` stays `finished`;
  - leaving the lobby and a finished table is built now; leaving mid-game stays in phase 5;
  - after the first cold review: fix 1, 2 and 4 to 9, record 3; after the second: fix 2 to 5,
    document 1; one `tsan` run; no third review.
- **Built.**
  - **Names** (`core/Names`), on utf8proc v2.12.0, a submodule built with the project. A name is
    composed (NFC), its spaces made plain, collapsed and trimmed, and its invisible characters
    dropped, except the joiners and variation selectors emoji need. It is refused for control,
    bidi or other formatting characters, for nothing visible (U+2800 and the Hangul fillers count as
    nothing), and past 20 code points. Names clash by a key that ignores case, width and invisible
    characters and counts Ş/Ţ as Ș/Ț. "LowRisk" and "Ducking" stay out of every name but a
    `low-risk` or `ducking` computer player's.
  - **The catalogues,** copied from the desktop at `cfeae4c` into `protocol/catalogues/` and
    compiled into the core (`core/Catalogue`). A person's portrait is kept for no level and is for
    their gender; a computer player's may be its level's own. A computer player the host sets gets
    a name and portrait by the desktop's rules, in the host's language.
  - **`Seating`,** the lobby's rules as plain data: join, leave and the host's hand-over, take a
    seat, set a seat, profiles and the host's settings. `Table` keeps it under its lock, with its
    lobby methods in `TableLobby.cpp`. The Registry makes, joins and leaves online tables.
  - **The round hold online:** after each scored round, until every person connected then
    continues, at most 30 s. Its state has one owner, the publisher's rest lock, and is set up before
    the result goes out, so a Continue sent on seeing it counts. `held` and `sync` carry the time
    left, and the hold is sent again as people continue.
  - **`tableState` on every change,** presence included, and **`/t/<code>`**, which serves the page.
  - **The debug page** makes, joins and runs online tables.
  - **`PROTOCOL.md`** describes all of it, with two new fixtures, `tableState.online` and
    `held.online`.
- **Tests:** 37 new core tests, 97 in all, and 2 new app tests, 17 in all. CTest runs 115.
  - **Core:** names, the catalogues and their draws, every seating rule, making, joining, leaving and
    the host's hand-over, a finished table as a lobby, presence, the turn timer's default, floor and
    off, every lobby message refused mid-game, and three people with three computer players to the
    end. The hold: on everyone's Continue, at its deadline, a Continue on `roundScored`, a late one,
    a disconnected person, a tab that closes during it, a sync, and closing. The leak scan gains an
    online game.
  - **Mutation checks:** a Continue for the wrong round, and a timer of 0 passed on, each fail.
  - **App:** the same game over three WebSocket clients, and `/t/<code>`.
  - **The browser,** by hand: three separate headless Chromium browsers made, joined (two by the
    link) and played a 6-seat 1-8-1 game through the debug page, 32 rounds with a hold after each,
    with no page errors.
- **Where the build departs from the plan, or found what it did not say:**
  - **The host cannot move or remove someone** (§7.2): §6.3 has no message for it. People move
    themselves.
  - **Online `updateSettings` reads `seats` for their number only.** What a seat holds changes
    through `setSeat`, `takeSeat` and `updateProfile`, so a host's change never fails on someone
    else's rename. Seats off the end must hold nobody.
  - **A clash on joining gets a number;** `updateProfile` to a taken name is `nameTaken`; a maker
    whose name clashes with their own computer players' is refused.
  - **Making and joining a table are answered with a `sync`.**
  - **The last one out closes the table at once;** §4.9's 10 minutes are for people who go away.
  - **Only messages that do not decode count toward the five that close a connection.**
  - **Online defaults:** Reckoner Beta stands in (D10), and the turn timer is 60 s (§7.2), at least
    10 s, with 0 for none.
  - **The game finds a member's seat itself,** and a sync carries a game only to someone in the
    seat they had in it.
  - **Lock order:** the publisher's rest lock may be held while its cache lock is taken, to send the
    hold; never the other way round.
  - **`/t/<code>`** is served by the server, and the debug page loads its files by absolute paths.
- **Left for later phases:**
  - **Phase 5:** until the grace period, an away person's turn is waited for as anyone's is, up to
    the turn timer each move, and for as long as it takes with no timer; nobody can leave a game
    under way, and a lobby whose host has gone cannot start. `waitingFor` and `replaced` come then.
  - **Phases 5 and 9:** still no reaper and no limits. **The server must not be public before
    them.**
  - **The page:** `sync.json` and `tableState.json` changed with the catalogue portraits.
- **Reviews.**
  - **The first cold review,** of the whole branch, found 9 problems, none above severity 3, and no
    race, deadlock or leaked card. The worst was the turn timer off by default, so one closed tab
    could stall a game nobody could leave. The owner chose my recommendation: fix 1, 2 and 4 to 9
    (`411b39a`), and record 3, the host moving people, as left out.
  - **The second cold review,** of that fix, found 5 problems, none above severity 3. The worst:
    with the timer off, an away person still holds the table. The owner chose my recommendation:
    fix 2 to 5 and document 1 (`ccdfd14`), with no third review.
- **Builds and CI.**
  - **`default` and `release`** (warnings as errors) pass 115 of 115 at `ccdfd14`.
  - **`tsan`,** one run with the owner's leave, passes 115 of 115 at `411b39a`, with no report. The
    last fix adds no code that runs on more than one thread.
  - **CI** is green at `87637db` and at `411b39a`.

### Phase 5: drops and replacement — M

- **Work:**
  - here, away, replaced and reclaimed, with pong-based detection;
  - the turn timer, `promptClosed`, Leave table;
  - the hold when every human is away;
  - the reaper.
- **Done when:**
  - every timeline of [§5.2](#52-timelines) passes as a core test;
  - the chaos test, run for an hour of games, finishes every game with no table stalled.

### Phase 6: the table page — L

In the page's plan, [§15](https://github.com/conectionist/romanian_whist_web/blob/master/docs/page-plan.md#phase-6-the-table-page--l).
In this repository it first builds the fixture tool (§10.5), on a phase branch of its own off this
repository's parent branch. That branch is reviewed like any phase's, and merged with a merge
commit before the page needs the fixtures. The page's submodule then moves to that merge.

### Phase 7: sheets and menus — L

In the page's plan, [§15](https://github.com/conectionist/romanian_whist_web/blob/master/docs/page-plan.md#phase-7-sheets-and-menus--l).

### Phase 8: Romanian, keyboard, touch — M

In the page's plan, [§15](https://github.com/conectionist/romanian_whist_web/blob/master/docs/page-plan.md#phase-8-romanian-keyboard-touch--m).

### Phase 9: hardening and deployment — M

- **Work:**
  - the limits and rate limits, the Content-Security-Policy;
  - the drain, `/health`, and `/version`, which the page's source links read;
  - the load test;
  - the server, Caddy, systemd, a domain;
  - the page's share, in `romanian_whist_web`: the footer's source links and the About page
    ([the page's plan, §12](https://github.com/conectionist/romanian_whist_web/blob/master/docs/page-plan.md#12-security)),
    and the page built and deployed as §12 says.
- **Done when:**
  - it is deployed;
  - the load test has set the running-game cap;
  - a restart drains cleanly.

### Phase 10 (optional): offline solo — M to L

In the page's plan, [§15](https://github.com/conectionist/romanian_whist_web/blob/master/docs/page-plan.md#phase-10-optional-offline-solo--m-to-l).
In this repository it compiles `server/core` with Emscripten, behind its two seams (§4.1). That
work goes on a phase branch of its own off this repository's parent branch, as phase 6's does, and
is merged before the page's part needs it.

### Phase 11 (optional): game records and recovery — M

- **Work:**
  - log each game's shuffle seed and every decision, as it happens;
  - after a restart, rebuild each game: replay its decisions with the observers silenced, then go
    on live. Stand-ins rebuild their memory by watching the replay, as they would watch a live game;
  - game records to download.
- **Done when:** after `kill -9` mid-game and a restart, every game resumes where it was.

---

## 14. Risks

| Risk | What keeps it small |
|---|---|
| A card reaches a player who may not see it | The engine's viewer-filtered snapshot is the only reader of hands (E1). The engine's leak property and the server's JSON scan |
| A deadlock or a race between game and I/O threads | The desktop client's patterns, ported; one lock order ([§4.6](#46-threads-and-locks)); `tsan` runs; the chaos test |
| A table stalls on a wait with no way out | Every wait has a deadline or a cancel (rule 4), with a test for each, and a log line when a wait outlives its deadline |
| A stand-in plays badly after taking over mid-round | Phase 0's stand-in tests: legal moves, and no Cyborg fallbacks |
| A restart loses games | The drain (phase 9), and recovery by replay (phase 11) |
| Reckoners load a small server | The running-game cap, the lower thread priority, and the measurements of phase 9 |
| Abuse: floods of tables or messages | The limits of [§9](#9-security-and-fairness) |
| Silent disconnects | Detection by pong, checked in phase 1 |
| Names that spoof or inject | Text-only rendering, the Content-Security-Policy, the name rules of [§7.3](#73-names-genders-and-portraits) |
| Drogon behaves differently from what this plan assumes | Phase 1's spikes, before anything depends on them |

The page's own risks, about its look, are in [its plan's §16](https://github.com/conectionist/romanian_whist_web/blob/master/docs/page-plan.md#16-risks).

---

## 15. Decisions, with defaults

The plan assumes each default until the owner decides otherwise.

| # | Question | Default |
|---|---|---|
| D1 | Where do solo games run? | On the server. Offline in the browser later (phase 10) |
| D2 | Where does the web client's code live? | **Decided by the owner, 2026-10-28:** a new repository, `romanian_whist_web`, this one. **Changed by the owner, 2026-11-03:** this repository is now `romanian_whist_server`, for the server, its protocol and `server/core`, which the Android app uses too. The page moved to a new `romanian_whist_web` the same day, with its part of this plan |
| D3 | Which Drogon? | 1.9.14, as a submodule built with the project |
| D4 | Which JSON library? | jsoncpp, which Drogon already uses |
| D5 | What is the page built with? | In [the page's plan, §17](https://github.com/conectionist/romanian_whist_web/blob/master/docs/page-plan.md#17-decisions-with-defaults) |
| D6 | Accounts? | None: a display name and a token in the browser |
| D7 | How are tables found? | By link or code only; no public list |
| D8 | How long is the grace period? | 30 s |
| D9 | The online turn timer? | 60 s, and three timed-out moves in a row count as a drop. The host may turn it off |
| D10 | Which computer player replaces a human? | The host chooses; Reckoner Beta by default |
| D11 | When does a returning player get the seat back? | At once, from the next decision |
| D12 | The online round hold? | Until every connected human presses Continue, at most 30 s |
| D13 | Pause in online games? | No |
| D14 | Phones? | In [the page's plan, §17](https://github.com/conectionist/romanian_whist_web/blob/master/docs/page-plan.md#17-decisions-with-defaults) |
| D15 | Spectators at others' tables? | Later, and never seeing hands while humans play |
| D16 | Seeds in online games? | Not offered |
| D17 | A solo player who goes away? | The game holds for up to 30 minutes |
| D18 | Which look? | In [the page's plan, §17](https://github.com/conectionist/romanian_whist_web/blob/master/docs/page-plan.md#17-decisions-with-defaults) |
| D19 | Chat? | None |
| D20 | Hosting? | One small server, with Caddy and systemd |

---

## 16. References

**The engine** (`romanian_whist_engine`)

- `README.md`: the observer contract, move providers, "Round types and who may see a hand",
  "Threading, stopping, and failure".
- `include/romanian_whist/GameEngine.h`, `IGameObserver.h`, `IMoveProvider.h`, `BetContext.h`,
  `PlayContext.h`.
- `include/romanian_whist/strategies/CyborgStrategy.h` and `ReckonerStrategy.h`: `setSeat()`,
  `makeCyborgSeat()` and `makeReckonerSeat()`.
- `docs/STRATEGIES.md` (strengths and costs), `docs/romanian-whist-reckoner-ai.md` (decision
  times), `docs/RULES.md`.

**The desktop client** (`romanian_whist_desktop`)

- `docs/QT_CLIENT_PLAN_proposal1.md`: §§3–6 (thread, bridge, provider, shutdown), §9 (the
  snapshot) and §10 (the clients compared).
- `src/core/GameSnapshot.h`, `src/core/Snapshot.cpp`: the snapshot and its traps.
- `src/engine/Prompts.h`, `src/engine/QtMoveProvider.cpp`: prompts, the generation gate,
  re-prompting.
- `src/engine/GameBridge.cpp`: publishing, pacing, the hold, pause.
- `src/engine/GameController.cpp`: the thread's life and its shutdown.
- `src/engine/RoundResult.h`: a scored round.
- `src/ui/NewGameDialog.cpp`: `BotLevel`, `buildDeal()`, `pacingFor()`.
- `src/ui/PlayerNames.cpp`, `src/paint/Avatars.cpp`, `resources/names/player_names.json`,
  `resources/images/avatars/avatars.json`: names and portraits.
- `src/ui/LastTrick.h`: the last trick, kept across rounds.

**The page** (`romanian_whist_web`)

- `docs/page-plan.md`: the page's plan, with its own references for the look.

**Outside**

- Drogon: [releases](https://github.com/drogonframework/drogon/releases),
  [WebSocket controllers](https://github.com/drogonframework/drogon/wiki/ENG-04-3-Controller-WebSocketController),
  [configuration](https://github.com/drogonframework/drogon/wiki/ENG-11-Configuration-File),
  [`HttpAppFramework.h`](https://github.com/drogonframework/drogon/blob/master/lib/inc/drogon/HttpAppFramework.h),
  [trantor's cross-thread `send()`](https://github.com/an-tao/trantor/blob/master/trantor/net/inner/TcpConnectionImpl.cc).
- Emscripten: [asynchronous code, JSPI and Asyncify](https://emscripten.org/docs/porting/asyncify.html).
- [caniuse: WebAssembly JSPI](https://caniuse.com/wf-wasm-jspi).
