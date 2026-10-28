# Web client mock-up

A clickable mock-up of the browser client planned in [`../web-client-plan.md`](../web-client-plan.md).
Open `web_client_mockup.html` in a browser. It needs a connection only for its fonts.

It shows eleven screens, picked from tabs above a browser window:

- **Before the game:** the main menu, New Game (solo), creating an online table, joining by
  link, and the lobby.
- **At the table, online:** a player who drops during his turn, your turn with the turn timer,
  a computer player standing in for someone who left, the round hold, the final results, and
  the "turn your phone" page.

A few things move: the countdowns run, a raised card can be played, the lobby's open seat can be
given to a computer player, and Continue works on the round sheet.

## What it follows

The table is the desktop client's. Paths below are in the desktop repository: the table space and
its fit to the window (`src/table/TableSpace.h`, `TableView.cpp`), the seat places and racks
(`SeatLayout.cpp`), the played cards round the logo and the lead mark (`TrickItem.cpp`), the hand
fan (`HandItem.cpp`), the hand order (`src/core/CardOrder.cpp`), the corner panels
(`TableScene::placeHud()`), and the colours (`src/paint/Theme.cpp`). The racks are the redesign
mock-up's CSS approximation, not the desktop client's projective ones.

The pieces neither client has (the lobby, presence on the name panels, the countdowns, the
notices) are drawn in the desktop client's style, as the plan's section 8.1 asks. They are a
first sketch, not decisions.

This is a sketch for discussion, not a reference. The plan's rule stands: the desktop client as
it runs decides how the web page looks.

## The art

The built page embeds the desktop client's art, from its `resources/`: the card faces
([Vector Playing Cards](https://opengameart.org/content/playing-cards-vector-png)), the player
portraits and the table textures. The fonts (Alegreya Sans, Belleza, Cinzel Decorative, under the
SIL Open Font License) load from Google Fonts.

## Rebuilding

```
python3 docs/web-client-mockup/source/build_mockup.py
```

`source/page.html` holds the markup and CSS, `source/mockup.js` the screens. The script takes the
art from the desktop client's checkout: a `romanian_whist_desktop` folder beside this repository,
or the path given with `--desktop`. Needs Pillow.
