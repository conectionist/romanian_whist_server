# The look fixtures

The states the browser page draws its tables from, to compare them with the desktop client's
screenshots, with no server running. Each file holds a `shot` header and a `state`. A state is **not
a message**: it is a `sync` without its `type` and `seq`, so the decoding tests of a client read
[`../fixtures/`](../fixtures/) only.

They are written by `tools/look/`, and `core_tests "[look]"` fails when any is stale, missing or
left over.

## Format

```json
{
  "shot":  { "file": "table-4-players.png", "width": 1920, "height": 1080, "view": "window" },
  "state": { "table": {}, "game": 1, "snapshot": {}, "history": [], "lastTrick": {}, "prompt": {}, "held": null }
}
```

- **`shot`** says which desktop picture the file pairs with.
  - `file`, `width` and `height` are the picture's name and size.
  - `view` is `"window"` for the whole desktop window with its menu bar, or `"table"` for the table
    alone, the window less its menu bar.
  - `raised` is the card under the cursor, as a card code. Only `hand-8-cards` has one.
- **`state`** is what the server would send as a `sync` to the person, who sits at seat 0. A client
  draws it as it draws a sync: the snapshot, the `prompt` for the legal cards and the bid strip, and
  `table.seats` for the faces and who is here. It is `null` for `table-empty`.

## The files

| File | Picture | Size | What it shows |
|---|---|---|---|
| `bidding.json` | `bidding.png` | 1920×1080 | 6 players, round 1 of 32, the first bid, 1 card in hand |
| `table-3-players.json` | `table-3-players.png` | 1920×1080 | Round 8 of 23, trick 2 of 6, 2 cards on the table, 5 in hand |
| `table-4-players.json` | `table-4-players.png` | 1920×1080 | Round 8 of 26, trick 2 of 5, 3 cards on the table, 4 in hand |
| `table-5-players.json` | `table-5-players.png` | 1920×1080 | Round 9 of 29, trick 2 of 5, 4 cards on the table, 4 in hand |
| `table-6-players.json` | `table-6-players.png` | 1920×1080 | Round 10 of 32, trick 2 of 5, 5 cards on the table, 4 in hand |
| `hand-1-card.json` | `hand-1-card.png` | 1920×1034 | 4 players, round 1 of 24: a hand of 1 card |
| `hand-4-cards.json` | `hand-4-cards.png` | 1920×1034 | 4 players, round 7 of 24: a hand of 4 cards |
| `hand-8-cards.json` | `hand-8-cards.png` | 1920×1034 | 4 players, round 11 of 24: a hand of 8 cards, the Queen of Hearts raised |
| `table-empty.json` | `table-empty.png` | 1920×1034 | A bare table: no `state` |

Every picture is taken on the person's turn. The sheets have no file here: the page builds them by
hand.

## How the games are defined

The tool plays the games of the desktop client's `tests/ScreenshotTool.cpp`, as it stands at commit
`cfeae4c` of `romanian_whist_desktop`, through a real `Service` and `Session`, so every state is
what the server sends.

- **The settings:** the person is "Player", a man. The opponents are Ana, Bogdan, Cristina, Dan and
  Elena, in that order, Low risk and Ducking by turns. The structure is 1-8-1, with the forehead
  and hidden rounds at the end. The deal's seed is 42, and the bots play at the top speed.
- **The person's rule:** a bid is the lowest the bid strip offers, and a card is the first legal one
  in the order the desktop shows a hand (the trump suit, then spades, hearts, clubs and diamonds,
  high before low), not in the engine's order.
- **The moment:** a picture is taken after a count of the person's decisions, bids and cards alike,
  and the tool checks that the round, the trick, the cards on the table and the cards in hand are
  the ones the desktop's tool expects. It writes nothing when they are not.
- **The faces** are dealt by the desktop's rule, from a generator seeded with 42.
- **The hands** are not games. They are built by hand, as the desktop's tool builds them: 4 players
  named You, Bogdan, Ana and Costel, wearing the first four faces of the catalogue, with the King
  of Hearts led by seat 1. Their table has no forehead and hidden rounds, so a game has the 24
  rounds the pictures say.

The engine the server builds on plays the same game as the desktop's. The pins differ only in the
snapshot and the Cyborg strategy. The deal and the table code use only a generator's raw output, so
they do not depend on the standard library. The faces do: the desktop draws them with
`std::uniform_int_distribution`, so a standard library other than libstdc++ could deal other faces,
and `[look]` would say so.

## Writing them again

```
nice -n 19 ./build/tools/look/whist_look
```

It prints each file with the moment it shows. Commit the difference. When the desktop's tool
changes a shot, change the tool by hand to match: it does not read the desktop's code.

Given a directory, the tool writes there and removes nothing. Without one it writes here, and also
removes any `.json` here that it no longer writes.
