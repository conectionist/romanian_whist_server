# The Romanian Whist protocol

Version **1**. This is the reference every client follows. The server's code is in
`server/core/Codec.cpp`. A real sample of every server message is in [`fixtures/`](fixtures/),
written by the server's tests (`core_tests "[fixtures]"`). A client's tests should decode each one.
The names and portraits players may have are in [`catalogues/`](catalogues/). The states the
browser page draws its tables from, for comparing its look with the desktop client's, are in
[`look/`](look/): each is a sync's fields without the envelope, not a message.

## Transport

- **One WebSocket per tab,** at `/ws`. Text frames only, one JSON object per frame, UTF-8.
- **Client messages are capped at 4 KiB.** A bigger one ends the connection with no Close frame,
  so the client sees close code 1006.
- **Every message has a `type`.**
- **Every server message also has `seq`.** It counts from 1 on each connection, and messages
  arrive in `seq` order. It is there for logs and tests; a client needs it for nothing else.
- **Silence.** A connection the server hears nothing from, not even a pong to its ping, for 30 s is
  closed by the server, with no Close frame, and its member is away (see
  [Going away](#going-away-and-coming-back)). Browsers answer pings below the page's script.
- **One connection per member.** A `hello` with a token that another connection holds takes the
  member over: the older connection gets `error {code: "openElsewhere"}` and is closed. A page
  that gets it should not reconnect by itself, or the two tabs would take the member from each
  other for ever.
- **The fifth message that does not decode closes the connection:** not JSON, an unknown type or
  field, a value of the wrong kind, anything before `hello`, a second `hello`, or a binary frame. A
  sound message the rules refuse never closes it, whatever its error.
- **No sentences on the wire.** Names, numbers and codes only; each client writes its own text.
- **Words.** The protocol says `bet`, as the engine does. Players see "bid".

## Values

| Value | Form |
|---|---|
| Card | Two characters: the rank `3`–`9`, `T`, `J`, `Q`, `K` or `A`, then the suit `H`, `D`, `S` or `C`. `"TH"` is the ten of hearts. (`2` exists in the code, but no deck deals it.) |
| Suit | `"H"`, `"D"`, `"S"` or `"C"` |
| Seat | A whole number from 0, in table order |
| Hand | An array of cards in the engine's order. A play answers with an index into it |
| Round index | 0-based. Show it as index + 1 |
| Level | `random`, `first-card`, `low-risk`, `ducking`, `cyborg`, `reckoner-alpha`, `reckoner-beta`, `reckoner-gamma`, `reckoner-delta` |
| Round type | `normal`, `forehead` (you see everyone's card but your own), `hidden` (nobody sees any) |
| Duration | Whole milliseconds |

### Profile

```json
{ "name": "Ana", "gender": "female", "avatar": "woman-long-red-hair" }
```

- `name`: **cleaned** by the server, then 1 to 20 characters. Cleaning composes it (NFC), makes every
  space a plain one, collapses runs of them and trims both ends, and drops invisible characters
  except the joiners and variation selectors emoji are made of. A name is refused when it holds a
  control or bidirectional formatting character, or any other formatting character, or when
  nothing in it is visible. The table shows the cleaned name.
- **Two names clash** when they match ignoring case, width (`Ａｎａ` is `Ana`) and invisible
  characters, with Romanian's cedilla letters counted as its comma-below ones (`Ştefan` is
  `Ștefan`). Names at a table may not clash.
- A name may not contain "LowRisk" or "Ducking", in any case or width, unless it is a `low-risk`
  or `ducking` computer player's: the Reckoner takes any seat so named for a deterministic player.
- `gender`: `female` or `male`.
- `avatar`: an `id` from [`catalogues/avatars.json`](catalogues/avatars.json) that suits the seat. A
  person's must be kept for no level (no `strategy`) and be for their gender, or for `any`. A
  computer player's may also be one kept for its own level.

### Settings

```json
{
  "seats": [
    { "kind": "human", "name": "Ana", "gender": "female", "avatar": "woman-long-red-hair" },
    { "kind": "computer", "level": "cyborg", "name": "Cyborg", "gender": "male", "avatar": "cyborg-man-eye" },
    { "kind": "open" }
  ],
  "structure": "1-8-1",
  "foreheadAndHidden": true,
  "allOneCardForehead": false,
  "botSpeed": 2,
  "waitAfterRound": false,
  "seed": 42,
  "replacement": "reckoner-beta",
  "turnTimer": 60000
}
```

| Field | Default when absent | Notes |
|---|---|---|
| `seats` | required | 2 to 6. A game starts only with every seat a human or a computer player |
| `structure` | required | `"1-8-1"` or `"8-1-8"` |
| `foreheadAndHidden` | `true` | End with one Forehead and one Hidden round |
| `allOneCardForehead` | `false` | Every one-card round is Forehead |
| `botSpeed` | `2` | 0 (slow) to 4 (instant) |
| `waitAfterRound` | `false` | Online, the round hold |
| `seed` | none | Solo only: a fixed deal. Online it is refused, since a seed is the whole deal |
| `replacement` | Online: `reckoner-beta` | Online only: the computer player that stands in for a human |
| `turnTimer` | Online: `60000` | Online only: how long a person's turn may take, 10000 to 3600000, or `0` for no timer |

`null` counts as absent. Names may not clash (see [Profile](#profile)), and each seat's portrait
must suit it.

## Client to server

Unknown types and unknown fields are refused with `error {code: "badMessage", detail}`, where
`detail` names the field at fault. Whole numbers must be written as whole numbers: `3.0` is refused.
An error goes back on the connection whose message caused it. The one exception comes from the
game: `illegal` for an invalid answer, and the prompt sent again with it, go to the member's
connection at that moment, which may be a newer tab.

| `type` | Fields | Allowed |
|---|---|---|
| `hello` | `protocol`, `token` (absent the first time; 22 base64url characters), `lang` (`en` or `ro`) | First, and only first |
| `createSolo` | `settings` | Not at a table |
| `createTable` | `settings`, `profile` | Not at a table |
| `joinTable` | `code` (6 characters, `A`–`Z` and `0`–`9`), `profile` | Not at a table |
| `updateProfile` | `name`, `gender`, `avatar` | Online, between games |
| `takeSeat` | `seat` | Online, between games, an open seat |
| `setSeat` | `seat`, `kind` (`open` or `computer`), `level` (with `computer`) | Host, online, between games |
| `updateSettings` | `settings`, whole | Host. Online, between games. Solo: speed and the wait at any time |
| `startGame` | – | Host, in the lobby or after a game |
| `bet` | `prompt`, `value` | Your own open prompt |
| `play` | `prompt`, `index` | Your own open prompt |
| `continue` | `round` | Once the round is scored: see [The round hold](#the-round-hold) |
| `pause`, `resume` | – | Solo, while a game is on |
| `leaveTable` | – | At a table. Solo: closes the table. Online: opens your seat between games; during a game a computer player takes it |
| `back` | – | At a table: see [Going away](#going-away-and-coming-back). Ignored when there is nothing to take back |


### hello

- **It comes first, and only once.** Anything before it, and a second one, is `badMessage`.
- **Another `protocol`** gets `error {code: "version"}`, and the connection is closed.
- **No `token`, or one the server does not know** (it has restarted since), makes a new member.
  Keep the `welcome`'s token, whichever it is.
- **The answer is a `welcome`, then always a `sync`:** with `table: null` when the member is at no
  table. Wait for the sync and render it; anything that came between the two is older than it.

### Solo, while a game is on

- **`pause`** holds the game at its next rest: after a computer player's bid, before its card, or
  on a finished trick or round. A move already under way finishes first. The game takes your own answers while
  paused, then holds at its next rest, so grey your buttons. `resume` lets it go on. Both are
  answered with a `tableState`, whose `paused` says which it is; pausing twice changes nothing and sends nothing.
- **`updateSettings`** takes the whole settings, of which only `botSpeed` and `waitAfterRound` may
  differ from the table's (`illegal` otherwise). A new speed applies at once, even to the rest under
  way. The wait applies from the next scored round. Answered with a `tableState`.
- **The wait after each round:** see [The round hold](#the-round-hold).
- **`startGame`**, once a game is over, starts another with the same settings, at once.
- **`leaveTable`** closes the table, game and all. You can then `createSolo` again.

### Online tables

A table's link is `/t/<code>`, which serves the page; the page reads the code from its address.

- **`createTable`** makes a table in its lobby, with you as its host in its first open seat.
  `settings.seats` holds `open` and `computer` seats only, at least one open; `seed` is refused.
  Your name may not clash with your own computer players' (`badMessage`, `name`). Answered with a
  `sync` of the table.
- **`joinTable`** seats you in the first open seat. A name already taken there gets a number: "Ana"
  joins as "Ana 2". Answered with a `sync`; everyone else gets a `tableState`.
  - `notFound`: no table has that code, or it is a solo table's.
  - `tableFull`: no seat is open, as while a game is on.
  - `illegal`: you are at a table already.
  - The profile is checked first, so a bad one is `badMessage` whatever the code.
- **Between games** means in the lobby, or once a game is over: a finished table is a lobby again,
  and its `state` stays `finished` until the next game starts. Then:
  - `takeSeat` moves you to an open seat (`illegal` for one that is not open, `badMessage` past
    the end).
  - `updateProfile` changes your name, gender and portrait; `nameTaken` when someone at the table
    has that name.
  - Nobody may move or remove someone else; people move themselves.
  - The host's `setSeat` makes a seat open, or a computer player of that level. The server gives a
    computer player its name and portrait from the catalogues, by the desktop client's rules: a
    name kept for its level first, else anyone's, in the host's `hello.lang`, else "Bot N"; its
    level's own portrait first, else anyone's, among those worn least at the table. A computer
    player whose level changes keeps its gender. A seat a person holds is `illegal`.
  - The host's `updateSettings` changes everything but what the seats hold. Of `settings.seats`
    only the number is read: seats added are open, and seats taken off the end must hold nobody
    (`illegal`, `seats`). Send the table's settings back with your changes.
  - The host's `startGame` needs every seat filled: `illegal`, `seats`, otherwise.
  - `leaveTable` opens your seat and answers with a `sync` with `table: null`. A host who leaves
    hands over to whoever has been there longest, and the last one out closes the table.
- **During a game** none of these are allowed (`illegal`) but `leaveTable`, and nobody new can join
  (`tableFull`). Someone who left the game can join again by its code: see
  [Going away](#going-away-and-coming-back).
- **Presence:** everyone is sent a `tableState` when someone's connection comes back or goes, with
  their seat's `presence` `here` or `away`. A connection that a newer tab replaced says nothing.

### Going away and coming back

Online, except where it says solo.

- **Away.** The member's connection has closed, or has said nothing for 30 s. Their seat is still
  theirs, and everyone is sent a `tableState` with its `presence` `away`.
- **The grace period** is 30 s from going away. If a turn of theirs comes inside it, the game waits
  for them until the earlier of the end of the grace period and their turn timer. `tableState`
  then has `waitingFor`, `{ "seat": 2, "deadline": 23000 }`: whose turn is waited for, and the
  milliseconds left when it was sent. A returning member's `sync` carries the prompt they had.
- **Replaced.** A computer player, the table's `replacement`, makes the seat's moves from now on,
  and the seat's `presence` is `replaced`. It keeps its name, portrait and score. This happens when
  the wait above runs out, when the grace period ends before a turn of theirs comes, when they leave,
  and when their turn timer runs out three times in a row while they are connected. A prompt open
  for them is closed with `promptClosed {by: "computer"}`, and a replaced seat is not awaited in the
  round hold.
- **Reclaimed.** A `hello` with their token, `back`, or any other message from them but
  `leaveTable` gives them the seat again from the next decision. A decision that the computer player
  is making finishes first. Everyone is sent a `tableState` with `presence` `here`.
- **Leaving** (`leaveTable` during a game) replaces the seat at once. The member is sent nothing
  more of the game, not even the `promptClosed` of a prompt they had, and is at no table: they may
  make or join another. Joining this table again by its code while the game is on gives them the
  seat back, with a `sync`; the profile sent is not used, since the engine fixed the names when the
  game started. Once the game is over their seat is open.
- **Nobody connected.** A game nobody at the table is connected to holds at its next rest, and goes
  on once someone is. A table nobody has been connected to for ten minutes is closed, game and all.
- **The host** who is away past the grace period hands over to whoever has been there longest and
  is connected.
- **Solo** never replaces the player. When they go away the game holds where it is, and goes on
  when they come back. The table is closed after thirty minutes of nobody being there.

### The round hold

With `waitAfterRound` on, every scored round, the last included, is followed by a `held`, and the
game waits for `continue {round}`.

- **Solo:** it waits for as long as it takes, for your Continue (or the watcher's).
- **Online:** it waits for the people connected when the round was scored, for at most 30 s. Each
  Continue takes its seat off the hold, and the `held` is sent again with the seats still awaited;
  the hold ends when nobody is left on it, or at its deadline. Someone whose connection goes is no
  longer awaited. Nobody connected means no hold.
- A `continue` may be sent as soon as the round's `roundScored` arrives, before its `held`, and
  still counts. A `continue` for a round that is not held, or from a seat it does not wait for, is
  ignored.

## Server to client

| `type` | Fields | To |
|---|---|---|
| `welcome` | `token`, `member`, `server` (`version`, `commit`) | The connection, first |
| `tableState` | `code`, `mode`, `state`, `host`, `settings`, `seats`, `paused`, `waitingFor` (only while waiting) | Everyone at the table, on every change |
| `game` | `game`, `event`, `snapshot` | Everyone at the table, each with their own snapshot |
| `prompt` | see [Prompts](#prompts) | The seat's member |
| `promptClosed` | `id`, `by` (`you` or `computer`) | The seat's member |
| `held` | `round`, `waitingFor` (seats), `deadline` | Everyone, during the round hold |
| `sync` | see [Sync](#sync) | After every `welcome`, and on making, joining or leaving an online table |
| `error` | `code`, sometimes `detail` | The connection |

### tableState

- `mode`: `solo` or `online`.
- `state`: `lobby`, `playing`, `finished` or `closed`.
- `host`: the host's seat, or `null` when the host has none (watching a solo game).
- `seats`, one of each kind:

```json
{ "kind": "human", "name": "Ana", "gender": "female", "avatar": "woman-long-red-hair",
  "presence": "away", "you": false }
{ "kind": "computer", "level": "cyborg", "name": "Cyborg", "gender": "male", "avatar": "cyborg-man-eye" }
{ "kind": "open" }
```

- `presence`: `here` or `away`, whether the person's connection is open, or `replaced` while a
  computer player plays their seat. It is sent again whenever it changes. See
  [Going away](#going-away-and-coming-back).
- `paused`: whether a solo game is paused. Always `false` online, and when no game is on.
- `waitingFor`: `{ "seat": 2, "deadline": 23000 }` while the game waits for the turn of a player
  who is away. `deadline` is the milliseconds left when it was sent: count down from when you
  receive it. Absent otherwise.
- `state` turns to `finished` only in the next `tableState`; a game's end is its `gameOver` event.
- **A closed table's `tableState` is the last message from it.** Nothing of its game follows.

### game

`game` is the game's number at the table, from 1. `event.kind` is one of the engine's thirteen
callbacks:

| `kind` | Other fields |
|---|---|
| `gameStarted`, `roundStarted`, `bettingComplete`, `roundComplete`, `gameStopped` | – |
| `betRequested`, `cardRequested` | `seat` |
| `betPlaced` | `seat`, `bet` |
| `trickStarted` | `trick` (1-based), `leader` |
| `cardPlayed` | `seat`, `card` |
| `trickWon` | `seat`, `trick` |
| `roundScored` | `result`, a [round result](#round-result) |
| `gameOver` | `standings`, `winners`: rows of `seat`, `name`, `score`, `place` |

`place` is a competition ranking: scores 50, 40, 40, 30 place 1, 2, 2, 4. `winners` holds every
seat sharing the top score; there is no tie-breaker.

A game that fails on the server ends with `gameStopped`, carrying the last snapshot sent, or `null`
if none was.

### Snapshot

The game as this viewer may see it, after the event:

```json
{
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
      "total": 41, "wins": 2, "losses": 0, "hand": ["AS", "TD"] }
  ]
}
```

- `you`: the viewer's seat, or `null` for a watcher.
- `status`: `notStarted`, `inProgress`, `finished` or `stopped`.
- `phase`: `notStarted`, `betting`, `playing`, `roundScored` or `gameOver`.
- `round.trick`: 1-based, 0 before the first trick. `round.trump`: `null` in 8-card rounds.
  `round.leader`: who opened the round's bidding.
- `active`: whose turn it is, or `null` between turns.
- `forbiddenBet`: the one bid the last bidder may not make, or `null`.
- `table.cards`: in play order. A finished trick stays until the next trick starts or the round
  ends. `table.ledBy` is who led those cards, or who leads next while there are none.
- `seats[i].bet`: `null` until that seat bids.
- `seats[i].roundScore` and `total`: during `roundScored`, `total` does not yet include the round.
- `seats[i].hand`: **present only when the viewer may see that hand.** Normal rounds: your own.
  Forehead: everyone's but yours. Hidden: nobody's. A watcher never sees one. `cards` always says
  how many backs to draw.

### Round result

```json
{ "index": 7, "count": 32, "tricks": 3, "type": "normal", "trump": "7S",
  "nextCountsForStreaks": true,
  "seats": [ { "bet": 1, "won": 1, "roundScore": 6, "total": 35, "wins": 3, "losses": 0 } ] }
```

- `total` does not include `roundScore`; the new total is their sum.
- `wins` and `losses` already count this round.
- `nextCountsForStreaks`: whether the next round deals more than one card. One-card rounds count
  for neither streak. `false` after the last round.

### Prompts

```json
{ "type": "prompt", "id": 57, "kind": "play", "deadline": 60000,
  "hand": ["AS", "TD"], "legal": [0], "blind": false }

{ "type": "prompt", "id": 58, "kind": "bet", "deadline": null,
  "hand": ["AS", "TD", "4C"], "max": 3, "forbidden": 1, "first": false, "roundType": "normal" }
```

- **A prompt carries everything its answer needs.** `legal` holds indices into `hand`; `max` is
  the highest bid; `forbidden` is the bid you may not make, or `null`.
- **A blind prompt has no `hand`,** only `handSize`: in Forehead and Hidden rounds you may not see
  your own card. `legal` still names the index to send.
- **`deadline`** is the milliseconds left when the prompt was sent, or `null` with no timer.
  Count down from when you received it.
- **Answer** with `bet {prompt, value}` or `play {prompt, index}`, using the prompt's `id`.

**A prompt is live from its `prompt` until its `promptClosed`, a `sync`, or the end of the game.**

- Every prompt the game settles ends with exactly one `promptClosed`: `by: "you"` when your answer
  was taken, `by: "computer"` when the timer ran out and your seat's computer player moved.
- **An invalid answer** gets `error {code: "illegal"}`, then the same prompt again, with the same
  `id` and the time left. The timer keeps running.
- **An answer to a prompt that is not open** gets `stalePrompt` (a prompt you had) or
  `notYourTurn`.
- **Prompt ids are never reused at a table.** A `prompt` whose `id` is already live is the same
  prompt again; a closed id stays closed.

The order, for one move: the `game` event asking for it (`betRequested` or `cardRequested`),
`prompt`, any (`error`, `prompt`) pairs, `promptClosed`, then the `game` event with the move.

### held

```json
{ "type": "held", "round": 0, "waitingFor": [0], "deadline": null }
```

- `round`: the round that was scored, 0-based like a round result's `index`.
- `waitingFor`: the seats whose `continue` is awaited. Empty in a watched solo game, which its
  watcher continues. Online it shrinks as people continue, and the `held` is sent again.
- `deadline`: the milliseconds left when it was sent, or `null` when there is no limit, as in
  solo. Count down from when you received it.
- **A hold ends with the game's next message** (`roundComplete`), or, when you `continue` while
  paused, with your `continue`: a `sync` then shows no hold.

### Sync

Sent to a member who joins or comes back. It holds everything needed to show the table now:

| Field | |
|---|---|
| `table` | The `tableState`, without `type` and `seq`; or `null` at no table |
| `game` | The game's number, or `null` before the first |
| `snapshot` | This viewer's latest snapshot, or `null` |
| `history` | One round result per scored round |
| `lastTrick` | `{ round, ledBy, cards }`, the last finished trick, kept across rounds; or `null` |
| `prompt` | Your open prompt, with `deadline` the time left now; or `null` |
| `held` | The round hold, as in `held`; or `null` |

Drop whatever was queued and render the sync. The `game` messages after it are newer than it.
A `prompt` after it with the sync's prompt `id` is that same prompt; a `promptClosed` for an `id`
you do not hold can be ignored.

## Errors

`error {code, detail}`. `detail` only ever names a field.

| Code | |
|---|---|
| `badMessage` | Not valid JSON, an unknown type or field, a value out of range, or a name, gender or portrait the rules refuse |
| `version` | `hello.protocol` is not this version. Reload the page |
| `notFound` | No such table, or no such member |
| `tableFull` | Every seat is taken |
| `nameTaken` | Someone at the table has that name |
| `notHost` | Only the host may do that |
| `notYourTurn` | You have no open prompt |
| `stalePrompt` | That prompt is no longer open |
| `illegal` | An answer the rules do not allow, or not allowed now: at a table already, at no table, during a game, a seat someone holds, a start with an open seat, and so on |
| `rateLimited` | Too many messages |
| `draining` | The server is restarting, or cannot start a game just now, and takes no new games |
| `openElsewhere` | Another connection has taken this member over; this one is closed |

## Catalogues

[`catalogues/player_names.json`](catalogues/player_names.json) and
[`catalogues/avatars.json`](catalogues/avatars.json) are the desktop client's, copied from
`romanian_whist_desktop` at `cfeae4c`. The server is built with them.

- **A name** has `name`, `gender` and `language` (`en`, `ro` or `any`), and a `strategy` when it is
  kept for computer players of that level.
- **A portrait** has `id`, `gender` (`female`, `male` or `any`), `file`, and a `strategy` when it is
  kept for that level. `id` is what `avatar` holds; the picture is the client's to draw.
