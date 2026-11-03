# The Romanian Whist protocol

Version **1**. This is the reference every client follows. The server's code is in
`server/core/Codec.cpp`. A real sample of every server message is in [`fixtures/`](fixtures/),
written by the server's tests (`core_tests "[fixtures]"`). A client's tests should decode each one.

## Transport

- **One WebSocket per tab,** at `/ws`. Text frames only, one JSON object per frame, UTF-8.
- **Client messages are capped at 4 KiB.** A bigger one ends the connection with no Close frame,
  so the client sees close code 1006.
- **Every message has a `type`.**
- **Every server message also has `seq`.** It counts from 1 on each connection, and messages
  arrive in `seq` order. It is there for logs and tests; a client needs it for nothing else.
- **One connection per member.** A `hello` with a token that another connection holds takes the
  member over: the older connection gets `error {code: "openElsewhere"}` and is closed. A page
  that gets it should not reconnect by itself, or the two tabs would take the member from each
  other for ever.
- **The fifth `badMessage` on a connection closes it.**
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

- `name`: 1 to 20 characters, no control or bidirectional formatting characters. A human's name
  may not contain "LowRisk" or "Ducking", in any case: the Reckoner takes any seat so named for a
  computer player of that kind.
- `gender`: `female` or `male`.
- `avatar`: a catalogue id: lower-case letters, digits and `-`, at most 40 characters.

### Settings

```json
{
  "seats": [
    { "kind": "human", "name": "Ana", "gender": "female", "avatar": "woman-long-red-hair" },
    { "kind": "computer", "level": "cyborg", "name": "T-800", "gender": "male", "avatar": "cyborg-man-eye" },
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
| `waitAfterRound` | `false` | |
| `seed` | none | Solo only: a fixed deal. Online it is refused, since a seed is the whole deal |
| `replacement` | none | Online only: the computer player that stands in for a human |
| `turnTimer` | none (off) | Online only, and only with a `replacement` |

`null` counts as absent. Names must differ from each other, ignoring case.

## Client to server

Unknown types and unknown fields are refused with `error {code: "badMessage", detail}`, where
`detail` names the field at fault. Whole numbers must be written as whole numbers: `3.0` is refused.
An error always goes back on the connection whose message caused it.

| `type` | Fields | Allowed |
|---|---|---|
| `hello` | `protocol`, `token` (absent the first time; 22 base64url characters), `lang` (`en` or `ro`) | First, and only first |
| `createSolo` | `settings` | Not at a table |
| `createTable` | `settings`, `profile` | Not at a table |
| `joinTable` | `code` (6 characters, `A`–`Z` and `0`–`9`), `profile` | Not at a table |
| `updateProfile` | `name`, `gender`, `avatar` | Lobby |
| `takeSeat` | `seat` | Lobby, an open seat |
| `setSeat` | `seat`, `kind` (`open` or `computer`), `level` (with `computer`) | Host, lobby |
| `updateSettings` | `settings`, whole | Host, lobby. Solo: speed and the wait at any time |
| `startGame` | – | Host, in the lobby or after a game |
| `bet` | `prompt`, `value` | Your own open prompt |
| `play` | `prompt`, `index` | Your own open prompt |
| `continue` | `round` | During the round hold |
| `pause`, `resume` | – | Solo, while a game is on |
| `leaveTable` | – | At a table. Solo: closes the table |

`createTable`, `joinTable`, `updateProfile`, `takeSeat` and `setSeat` are answered `illegal` until
the server has online tables.

### hello

- **It comes first, and only once.** Anything before it, and a second one, is `badMessage`.
- **Another `protocol`** gets `error {code: "version"}`, and the connection is closed.
- **No `token`, or one the server does not know** (it has restarted since), makes a new member.
  Keep the `welcome`'s token, whichever it is.
- **The answer is a `welcome`, then always a `sync`:** with `table: null` when the member is at no
  table. Wait for the sync and render it; anything that came between the two is older than it.

### Solo, while a game is on

- **`pause`** holds the game at its next rest: after a computer player's move, or on a finished
  trick or round. A move already under way finishes first. The game takes your own answers while
  paused, then holds at its next rest, so grey your buttons. `resume` lets it go on. Both are
  answered with a `tableState`, whose `paused` says which it is; pausing twice changes nothing and sends nothing.
- **`updateSettings`** takes the whole settings, of which only `botSpeed` and `waitAfterRound` may
  differ from the table's (`illegal` otherwise). A new speed applies at once, even to the rest under
  way. The wait applies from the next scored round. Answered with a `tableState`.
- **The wait after each round.** With `waitAfterRound` on, every scored round, the last included,
  is followed by a `held` and the game waits for your `continue` with that round. A `continue` for
  a round that is not held is ignored.
- **`startGame`**, once a game is over, starts another with the same settings, at once.
- **`leaveTable`** closes the table, game and all. You can then `createSolo` again.

## Server to client

| `type` | Fields | To |
|---|---|---|
| `welcome` | `token`, `member`, `server` (`version`, `commit`) | The connection, first |
| `tableState` | `code`, `mode`, `state`, `host`, `settings`, `seats`, `paused`, `waitingFor` (only while waiting) | Everyone at the table, on every change |
| `game` | `game`, `event`, `snapshot` | Everyone at the table, each with their own snapshot |
| `prompt` | see [Prompts](#prompts) | The seat's member |
| `promptClosed` | `id`, `by` (`you` or `computer`) | The seat's member |
| `held` | `round`, `waitingFor` (seats), `deadline` | Everyone, during the round hold |
| `sync` | see [Sync](#sync) | After every `welcome`, and on joining |
| `error` | `code`, sometimes `detail` | The connection |

### tableState

- `mode`: `solo` or `online`.
- `state`: `lobby`, `playing`, `finished` or `closed`.
- `host`: the host's seat, or `null` when the host has none (watching a solo game).
- `seats`, one of each kind:

```json
{ "kind": "human", "name": "Ana", "gender": "female", "avatar": "woman-long-red-hair",
  "presence": "away", "you": false }
{ "kind": "computer", "level": "cyborg", "name": "T-800", "gender": "male", "avatar": "cyborg-man-eye" }
{ "kind": "open" }
```

- `presence`: `here`, `away` or `replaced`.
- `paused`: whether a solo game is paused. Always `false` online, and when no game is on.
- `waitingFor`: `{ "seat": 2, "deadline": 23000 }` while the table waits for an away player's turn.
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
  watcher continues.
- `deadline`: the milliseconds left, or `null` when there is no limit, as in solo.
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
| `badMessage` | Not valid JSON, an unknown type or field, or a value out of range |
| `version` | `hello.protocol` is not this version. Reload the page |
| `notFound` | No such table, or no such member |
| `tableFull` | Every seat is taken |
| `nameTaken` | Someone at the table has that name |
| `notHost` | Only the host may do that |
| `notYourTurn` | You have no open prompt |
| `stalePrompt` | That prompt is no longer open |
| `illegal` | An answer the rules do not allow, or not allowed now: at a table already, at no table, not in an online game, and so on |
| `rateLimited` | Too many messages |
| `draining` | The server is restarting, or cannot start a game just now, and takes no new games |
| `openElsewhere` | Another connection has taken this member over; this one is closed |
