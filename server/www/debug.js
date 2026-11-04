// A plain page for trying the server: it shows what the server sends and answers its prompts.
// Text from the server only ever goes in through textContent.

const protocol = 1;
const tokenKey = "whist.debug.token";
const reloadedKey = "whist.debug.reloaded";

const $ = (id) => document.getElementById(id);

// Portraits are catalogue ids (protocol/catalogues/avatars.json), and a person's must suit them.
const names = ["Ana", "Bogdan", "Cora", "Dan", "Ema", "Filip"];
const you = { kind: "human", name: "You", gender: "female", avatar: "woman-long-red-hair" };

function computer(level, i) {
  const female = i % 2 === 0;
  return { kind: "computer", level, name: names[i], gender: female ? "female" : "male",
           avatar: female ? "woman-long-brown-hair" : "man-hat-mustache" };
}

const presets = {
  "Play against 3 Cyborgs": {
    seats: [you, computer("cyborg", 1), computer("cyborg", 2), computer("cyborg", 3)],
    structure: "1-8-1", botSpeed: 2, waitAfterRound: false,
  },
  "Play 6 seats of mixed levels": {
    seats: [you, computer("random", 1), computer("low-risk", 2), computer("ducking", 3),
            computer("reckoner-alpha", 4), computer("reckoner-beta", 5)],
    structure: "1-8-1", botSpeed: 3, waitAfterRound: false,
  },
  "Play 3 seats, 8-1-8, every one-card round Forehead, wait after each round": {
    seats: [you, computer("first-card", 1), computer("cyborg", 2)],
    structure: "8-1-8", allOneCardForehead: true, botSpeed: 3, waitAfterRound: true,
  },
  "Play 2 seats with a fixed deal": {
    seats: [you, computer("reckoner-gamma", 1)],
    structure: "1-8-1", botSpeed: 2, waitAfterRound: false, seed: 42,
  },
  "Watch 4 Reckoners": {
    seats: [computer("reckoner-alpha", 0), computer("reckoner-beta", 1), computer("reckoner-gamma", 2),
            computer("reckoner-delta", 3)],
    structure: "1-8-1", botSpeed: 3, waitAfterRound: false,
  },
  "Watch 3 at Instant": {
    seats: [computer("cyborg", 0), computer("random", 1), computer("first-card", 2)],
    structure: "1-8-1", botSpeed: 4, waitAfterRound: false,
  },
};

// A person's portraits: those in protocol/catalogues/avatars.json kept for no level, by gender.
const portraits = {
  female: ["woman-long-red-hair", "woman-long-brown-hair", "woman-long-dark-hair", "woman-shoulder-hair",
           "woman-wavy-black-hair"],
  male: ["man-dark-hair-beard", "man-hat-mustache", "man-white-hair-hat", "man-black-hair-beard", "man-stubble",
         "man-short-hair", "man-black-tee"],
};

const open = { kind: "open" };

// People join an online table's open seats; the table's maker takes the first.
const onlinePresets = {
  "6 seats: 3 people, 3 computer players, waiting after each round": {
    seats: [open, open, open, computer("reckoner-beta", 3), computer("cyborg", 4), computer("first-card", 5)],
    structure: "1-8-1", botSpeed: 3, waitAfterRound: true, replacement: "reckoner-beta", turnTimer: 60000,
  },
  "4 seats, all open, 8-1-8": {
    seats: [open, open, open, open],
    structure: "8-1-8", botSpeed: 2, waitAfterRound: true,
  },
  "3 seats: 2 people and a Cyborg, Instant, no hold": {
    seats: [open, open, computer("cyborg", 2)],
    structure: "1-8-1", botSpeed: 4, waitAfterRound: false,
  },
};

// The code in a table's link, /t/K7QM4P.
const linkCode = location.pathname.match(/^\/t\/([A-Z0-9]{6})$/)?.[1] ?? "";

const state = {
  socket: null,
  reconnect: true,
  waitingForSync: false,
  table: null,
  game: null,
  snapshot: null,
  prompt: null,
  held: null,
  history: [],
  lastTrick: null,
  events: [],

  // A game that stopped, by a failure on the server, is over: nothing else will say so.
  stopped: false,
};

function storage(kind) {
  try {
    return kind === "local" ? window.localStorage : window.sessionStorage;
  } catch {
    return null;
  }
}

function readToken() {
  try {
    return storage("local")?.getItem(tokenKey) ?? null;
  } catch {
    return null;
  }
}

function writeToken(token) {
  try {
    storage("local")?.setItem(tokenKey, token);
  } catch {
    // The token then lasts as long as this page.
  }
}

function send(message) {
  if (state.socket?.readyState === WebSocket.OPEN) {
    state.socket.send(JSON.stringify(message));
  }
}

// ---- the connection ----

function connect() {
  const scheme = location.protocol === "https:" ? "wss:" : "ws:";
  const socket = new WebSocket(`${scheme}//${location.host}/ws`);
  state.socket = socket;
  setStatus("connecting");

  socket.addEventListener("open", () => {
    setStatus("connected");
    const hello = { type: "hello", protocol, lang: "en" };
    const token = readToken();
    if (token) {
      hello.token = token;
    }
    send(hello);
  });

  socket.addEventListener("message", (event) => {
    let message;
    try {
      message = JSON.parse(event.data);
    } catch {
      addError(`not JSON: ${event.data}`);
      return;
    }
    receive(message);
    render();
  });

  socket.addEventListener("close", (event) => {
    if (state.socket !== socket) {
      return;
    }
    setStatus(`closed (${event.code})${state.reconnect ? ", reconnecting" : ""}`);
    if (state.reconnect) {
      setTimeout(connect, 1000);
    }
  });
}

function receive(message) {
  switch (message.type) {
    case "welcome":
      writeToken(message.token);
      try {
        storage("session")?.removeItem(reloadedKey);
      } catch {
        // Then a later deploy finds the flag still set, and asks for a reload by hand.
      }
      state.waitingForSync = true;
      setStatus(`connected as member ${message.member}, server ${message.server.version} ${message.server.commit}`);
      return;
    case "sync":
      state.waitingForSync = false;
      state.table = message.table;
      state.game = message.game;
      state.snapshot = message.snapshot;
      state.prompt = message.prompt;
      state.held = message.held;
      state.history = message.history;
      state.lastTrick = message.lastTrick;
      state.events = [];
      return;
    case "error":
      receiveError(message);
      return;
    default:
      break;
  }

  // Everything before the sync is older than it.
  if (state.waitingForSync) {
    return;
  }

  switch (message.type) {
    case "tableState":
      if (state.table?.code !== message.code) {
        clearGame();
      }
      state.table = message;
      if (message.state === "closed") {
        state.table = null;
        clearGame();
      }
      break;
    case "game":
      receiveGame(message);
      break;
    case "prompt":
      state.prompt = message;
      break;
    case "promptClosed":
      if (state.prompt?.id === message.id) {
        state.prompt = null;
      }
      addEvent(`prompt ${message.id} closed by ${message.by}`);
      break;
    case "held":
      state.held = message;
      break;
    default:
      addError(`unknown message ${message.type}`);
  }
}

function receiveGame(message) {
  if (state.game !== message.game) {
    clearGame();
    state.game = message.game;
  }
  const event = message.event;
  state.snapshot = message.snapshot;

  // A hold ends with the game's next message.
  state.held = null;

  if (event.kind === "roundScored") {
    state.history.push(event.result);
  }
  if (event.kind === "trickWon" && message.snapshot) {
    const table = message.snapshot.table;
    state.lastTrick = { round: message.snapshot.round.index, ledBy: table.ledBy, cards: table.cards };
  }
  if (event.kind === "gameOver" || event.kind === "gameStopped") {
    state.prompt = null;
    state.stopped = event.kind === "gameStopped";
  }
  addEvent(describeEvent(event));
}

function receiveError(message) {
  addError(`${message.code}${message.detail ? ` (${message.detail})` : ""}`);
  if (message.code === "openElsewhere") {
    // Reconnecting would take the game back from the other tab, which would then do the same.
    state.reconnect = false;
    setStatus("this game is open in another tab");
  }
  if (message.code === "version") {
    const session = storage("session");
    if (session && !session.getItem(reloadedKey)) {
      session.setItem(reloadedKey, "1");
      location.reload();
    } else {
      state.reconnect = false;
    }
  }
}

function clearGame() {
  state.game = null;
  state.stopped = false;
  state.snapshot = null;
  state.prompt = null;
  state.held = null;
  state.history = [];
  state.lastTrick = null;
  state.events = [];
}

// ---- words ----

const suits = { H: "♥", D: "♦", S: "♠", C: "♣" };

function card(text) {
  if (!text) {
    return "-";
  }
  const rank = text[0] === "T" ? "10" : text[0];
  return `${rank}${suits[text[1]] ?? text[1]}`;
}

function seatName(seat) {
  if (seat === null || seat === undefined) {
    return "-";
  }
  return state.snapshot?.seats[seat]?.name ?? state.table?.seats[seat]?.name ?? `seat ${seat}`;
}

function describeEvent(event) {
  const who = seatName(event.seat);
  switch (event.kind) {
    case "betRequested": return `${who} to bid`;
    case "betPlaced": return `${who} bids ${event.bet}`;
    case "trickStarted": return `trick ${event.trick}, ${who} leads`;
    case "cardRequested": return `${who} to play`;
    case "cardPlayed": return `${who} plays ${card(event.card)}`;
    case "trickWon": return `${who} wins trick ${event.trick}`;
    case "roundScored": return `round ${event.result.index + 1} scored`;
    case "gameOver":
      return `game over: ${event.standings.map((row) => `${row.place}. ${row.name} ${row.score}`).join(", ")}`;
    default: return event.kind;
  }
}

function pad(text, width) {
  return String(text).padEnd(width);
}

function snapshotText(snapshot) {
  if (!snapshot) {
    return "No game yet.";
  }
  const round = snapshot.round;
  const lines = [];
  lines.push(`Round ${round.index + 1} of ${round.count} · ${round.type} · ${round.tricks} card(s) · ` +
             `trick ${round.trick} · trump ${card(round.trump)} · ${snapshot.phase} (${snapshot.status})`);
  lines.push(`Bidding opened by ${seatName(round.leader)} · to move: ${seatName(snapshot.active)} · ` +
             `forbidden bid: ${snapshot.forbiddenBet ?? "-"} · bids ${snapshot.bets.placed} placed, sum ${snapshot.bets.sum}`);
  const table = snapshot.table;
  const played = table.cards.map((p) => `${seatName(p.seat)} ${card(p.card)}${p.winning ? "*" : ""}`).join(", ");
  lines.push(`On the table, led by ${seatName(table.ledBy)}${table.leadSuit ? ` (${suits[table.leadSuit]})` : ""}: ` +
             `${played || "nothing"}`);
  lines.push("");
  lines.push(`  ${pad("seat", 10)} ${pad("bid", 4)} ${pad("won", 4)} ${pad("cards", 6)} ${pad("round", 6)} ` +
             `${pad("total", 6)} ${pad("W/L", 6)} hand`);
  snapshot.seats.forEach((seat, i) => {
    const marker = i === snapshot.you ? ">" : i === snapshot.active ? "*" : " ";
    const hand = seat.hand ? seat.hand.map(card).join(" ") : "";
    lines.push(`${marker} ${pad(seat.name, 10)} ${pad(seat.bet ?? "-", 4)} ${pad(seat.won, 4)} ${pad(seat.cards, 6)} ` +
               `${pad(seat.roundScore, 6)} ${pad(seat.total, 6)} ${pad(`${seat.wins}/${seat.losses}`, 6)} ${hand}`);
  });
  return lines.join("\n");
}

function historyText() {
  if (state.history.length === 0) {
    return "No round scored yet.";
  }
  return state.history.map((result) => {
    const seats = result.seats.map((seat, i) =>
      `${seatName(i)} ${seat.bet ?? "-"}/${seat.won} ${seat.roundScore >= 0 ? "+" : ""}${seat.roundScore} = ` +
      `${seat.total + seat.roundScore}`).join(" · ");
    return `${pad(result.index + 1, 3)} ${pad(result.type, 9)} ${pad(card(result.trump), 4)} ${seats}`;
  }).join("\n");
}

function lastTrickText() {
  const trick = state.lastTrick;
  if (!trick) {
    return "None yet.";
  }
  const cards = trick.cards.map((p) => `${seatName(p.seat)} ${card(p.card)}${p.winning ? "*" : ""}`);
  return `Round ${trick.round + 1}, led by ${seatName(trick.ledBy)}: ${cards.join(", ")}`;
}

// ---- drawing ----

function setStatus(text) {
  $("status").textContent = text;
}

function addEvent(text) {
  state.events.push(text);
  if (state.events.length > 200) {
    state.events.shift();
  }
}

function addError(text) {
  const item = document.createElement("li");
  item.textContent = text;
  $("errors").prepend(item);
}

function button(label, enabled, onClick) {
  const element = document.createElement("button");
  element.type = "button";
  element.textContent = label;
  element.disabled = !enabled;
  element.addEventListener("click", onClick);
  return element;
}

function renderPrompt(paused) {
  const prompt = state.prompt;
  $("prompt").hidden = !prompt;
  const choices = $("choices");
  choices.replaceChildren();
  if (!prompt) {
    return;
  }

  const deadline = prompt.deadline === null ? "" : `, ${Math.round(prompt.deadline / 1000)} s`;
  if (prompt.kind === "bet") {
    $("promptText").textContent = `Your bid (0 to ${prompt.max}${prompt.forbidden === null ? "" :
                                   `, not ${prompt.forbidden}`}${deadline}):`;
    for (let value = 0; value <= prompt.max; value++) {
      if (value !== prompt.forbidden) {
        choices.append(button(String(value), !paused, () => send({ type: "bet", prompt: prompt.id, value })));
      }
    }
    return;
  }

  $("promptText").textContent = `Your card${prompt.blind ? " (you cannot see your hand this round)" : ""}${deadline}:`;
  const size = prompt.hand ? prompt.hand.length : prompt.handSize;
  for (let index = 0; index < size; index++) {
    const label = prompt.hand ? card(prompt.hand[index]) : `card ${index + 1}`;
    const legal = prompt.legal.includes(index);
    choices.append(button(label, legal && !paused, () => send({ type: "play", prompt: prompt.id, index })));
  }
}

function profile() {
  return { name: $("name").value, gender: $("gender").value, avatar: $("avatar").value };
}

function fillPortraits() {
  const select = $("avatar");
  const chosen = select.value;
  select.replaceChildren(...portraits[$("gender").value].map((id) => {
    const option = document.createElement("option");
    option.value = id;
    option.textContent = id;
    return option;
  }));
  if (portraits[$("gender").value].includes(chosen)) {
    select.value = chosen;
  }
}

function seatText(seat, i, table) {
  const host = table.host === i ? ", host" : "";
  switch (seat.kind) {
    case "open": return "open";
    case "computer": return `${seat.name} (computer: ${seat.level}, ${seat.gender}, ${seat.avatar})`;
    default:
      return `${seat.name} (${seat.presence}, ${seat.gender}, ${seat.avatar}${host})${seat.you ? " — you" : ""}`;
  }
}

function renderLobby(table, yours) {
  const online = table.mode === "online";
  $("lobby").hidden = !online;
  if (!online) {
    return;
  }

  const link = `${location.origin}/t/${table.code}`;
  $("link").href = link;
  $("link").textContent = link;

  const betweenGames = table.state === "lobby" || table.state === "finished";
  const hosting = yours !== null && table.host === yours;
  $("seats").replaceChildren(...table.seats.map((seat, i) => {
    const item = document.createElement("li");
    item.append(seatText(seat, i, table), " ");
    if (betweenGames && seat.kind === "open") {
      item.append(button("Take this seat", true, () => send({ type: "takeSeat", seat: i })));
    }
    if (betweenGames && hosting && seat.kind !== "human") {
      const level = document.createElement("select");
      for (const name of ["random", "first-card", "low-risk", "ducking", "cyborg", "reckoner-alpha",
                          "reckoner-beta", "reckoner-gamma", "reckoner-delta"]) {
        const option = document.createElement("option");
        option.value = name;
        option.textContent = name;
        level.append(option);
      }
      level.value = seat.level ?? "reckoner-beta";
      item.append(level, " ",
                  button("Computer", true, () => send({ type: "setSeat", seat: i, kind: "computer", level: level.value })),
                  " ", button("Open", seat.kind !== "open", () => send({ type: "setSeat", seat: i, kind: "open" })));
    }
    return item;
  }));

  $("hostControls").hidden = !hosting;
  $("update").disabled = !betweenGames;
  $("startOnline").disabled = !betweenGames;
  $("profile").disabled = !betweenGames;
  if (document.activeElement !== $("tableSettings")) {
    $("tableSettings").value = JSON.stringify(table.settings, null, 2);
  }
}

function render() {
  const table = state.table;
  const atTable = table !== null && !state.waitingForSync;
  $("you").hidden = state.waitingForSync || (table !== null && table.mode !== "online");
  $("setup").hidden = state.waitingForSync || table !== null;
  $("online").hidden = state.waitingForSync || table !== null;
  $("table").hidden = !atTable;
  if (!atTable) {
    return;
  }

  const playing = table.state === "playing";
  const over = state.snapshot?.phase === "gameOver" || state.snapshot?.status === "stopped" ||
               state.stopped || table.state === "finished";
  const yours = table.seats.findIndex((seat) => seat.you);
  const seat = yours < 0 ? null : yours;
  $("code").textContent = table.code;
  $("summary").textContent = `${table.mode} · ${table.state}${table.paused ? " · paused" : ""} · game ${state.game ?? "-"}` +
                             ` · ${seat === null ? "watching" : `you are seat ${seat}`}`;

  const solo = table.mode === "solo";
  $("soloControls").hidden = !solo;
  $("pause").textContent = table.paused ? "Resume" : "Pause";
  $("pause").disabled = !playing || over;
  $("speed").value = String(table.settings.botSpeed);
  $("wait").checked = table.settings.waitAfterRound;
  $("again").hidden = !over || !solo;
  renderLobby(table, seat);

  $("held").hidden = !state.held;
  if (state.held) {
    const waiting = state.held.waitingFor.map(seatName).join(", ");
    const deadline = state.held.deadline === null ? "" : `, at most ${Math.round(state.held.deadline / 1000)} s`;
    $("heldText").textContent = `Round ${state.held.round + 1} is over.` +
                                (waiting ? ` Waiting for ${waiting}${deadline}.` : "");
    $("continue").disabled = seat !== null && !state.held.waitingFor.includes(seat);
  }

  renderPrompt(table.paused);
  $("snapshot").textContent = snapshotText(state.snapshot);
  $("lastTrick").textContent = lastTrickText();
  $("history").textContent = historyText();

  const events = $("events");
  events.replaceChildren(...state.events.slice(-40).reverse().map((text) => {
    const item = document.createElement("li");
    item.textContent = text;
    return item;
  }));
}

function changeSettings(change) {
  if (!state.table) {
    return;
  }
  send({ type: "updateSettings", settings: { ...state.table.settings, ...change } });
}

function setUp() {
  const select = $("preset");
  for (const name of Object.keys(presets)) {
    const option = document.createElement("option");
    option.value = name;
    option.textContent = name;
    select.append(option);
  }
  const showPreset = () => {
    $("settings").value = JSON.stringify(presets[select.value], null, 2);
  };
  select.addEventListener("change", showPreset);
  showPreset();

  $("start").addEventListener("click", () => {
    let settings;
    try {
      settings = JSON.parse($("settings").value);
    } catch (error) {
      addError(`the settings are not JSON: ${error.message}`);
      return;
    }
    send({ type: "createSolo", settings });
  });

  const onlineSelect = $("onlinePreset");
  for (const name of Object.keys(onlinePresets)) {
    const option = document.createElement("option");
    option.value = name;
    option.textContent = name;
    onlineSelect.append(option);
  }
  const showOnlinePreset = () => {
    $("onlineSettings").value = JSON.stringify(onlinePresets[onlineSelect.value], null, 2);
  };
  onlineSelect.addEventListener("change", showOnlinePreset);
  showOnlinePreset();

  $("gender").addEventListener("change", fillPortraits);
  fillPortraits();
  $("joinCode").value = linkCode;

  $("create").addEventListener("click", () => {
    let settings;
    try {
      settings = JSON.parse($("onlineSettings").value);
    } catch (error) {
      addError(`the settings are not JSON: ${error.message}`);
      return;
    }
    send({ type: "createTable", settings, profile: profile() });
  });
  $("join").addEventListener("click", () => {
    send({ type: "joinTable", code: $("joinCode").value.trim().toUpperCase(), profile: profile() });
  });
  $("update").addEventListener("click", () => {
    let settings;
    try {
      settings = JSON.parse($("tableSettings").value);
    } catch (error) {
      addError(`the settings are not JSON: ${error.message}`);
      return;
    }
    send({ type: "updateSettings", settings });
  });
  $("startOnline").addEventListener("click", () => send({ type: "startGame" }));
  $("profile").addEventListener("click", () => send({ type: "updateProfile", ...profile() }));

  $("pause").addEventListener("click", () => send({ type: state.table?.paused ? "resume" : "pause" }));
  $("speed").addEventListener("change", () => changeSettings({ botSpeed: Number($("speed").value) }));
  $("wait").addEventListener("change", () => changeSettings({ waitAfterRound: $("wait").checked }));
  $("again").addEventListener("click", () => send({ type: "startGame" }));
  $("leave").addEventListener("click", () => send({ type: "leaveTable" }));
  $("continue").addEventListener("click", () => {
    if (state.held) {
      send({ type: "continue", round: state.held.round });
    }
  });
}

setUp();
render();
connect();
