#!/usr/bin/env node
// A few online tables of people who answer, disconnect, come back, leave and join again at random,
// against a running server, to see that every game finishes and no table stalls.
//
//   nice -n 19 node tools/chaos/chaos.mjs --server build/server/app/whist_server --minutes 60
//   nice -n 19 node tools/chaos/chaos.mjs --url ws://127.0.0.1:8080/ws --minutes 5
//
// With --server it starts that program with a configuration of its own (short grace, ping and
// pong timeouts) and stops it at the end. With --url the server is yours, and its grace period
// should be short too, and its pong timeout under 10 s. Node 22 or later, no packages.
//
// It passes when, after the last disruption, every table finishes a game, and:
//   - no table went without progress for longer than any wait of the server's allows;
//   - every tab that went silent without closing was closed by the server;
//   - no snapshot held a hand its viewer may not see, and no blind prompt held one;
//   - every message parsed, and the server sent no error a person could not have caused.
// The exit status is 0 on a pass and 1 otherwise.

import { spawn } from "node:child_process";
import { mkdtempSync, rmSync, writeFileSync } from "node:fs";
import { connect, createServer } from "node:net";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { pathToFileURL } from "node:url";
import { parseArgs } from "node:util";

// ---- what is checked ----

// A hand may be in a snapshot only when its viewer may see it: their own in a normal round, every
// other one in a Forehead round, none in a Hidden round, and none for a watcher.
export function checkSnapshot(snapshot) {
  const problems = [];
  if (!snapshot || typeof snapshot !== "object") {
    return problems;
  }
  const type = snapshot.round?.type;
  snapshot.seats?.forEach((seat, index) => {
    if (!("hand" in seat)) {
      return;
    }
    const mine = snapshot.you === index;
    const allowed = snapshot.you !== null && snapshot.you !== undefined &&
                    ((type === "normal" && mine) || (type === "forehead" && !mine));
    if (!allowed) {
      problems.push(`seat ${index}'s hand in a ${type} round, seen from ${snapshot.you}`);
    } else if (seat.hand.length !== seat.cards) {
      problems.push(`seat ${index} has ${seat.cards} cards but ${seat.hand.length} in its hand`);
    }
  });
  return problems;
}

// A prompt carries the hand unless the round is blind to its player. A play prompt says so with
// `blind`; a bet prompt, with a `roundType` of forehead or hidden.
export function checkPrompt(prompt) {
  const problems = [];
  const blind = prompt.kind === "play" ? prompt.blind === true : prompt.roundType !== "normal";
  if (blind && "hand" in prompt) {
    problems.push(`blind prompt ${prompt.id} carries a hand`);
  }
  if (!blind && !Array.isArray(prompt.hand)) {
    problems.push(`prompt ${prompt.id} carries no hand`);
  }
  const size = prompt.hand?.length ?? prompt.handSize;
  if (prompt.kind === "play" && prompt.legal.some((index) => !(index < size))) {
    problems.push(`prompt ${prompt.id} has a legal index outside the hand`);
  }
  return problems;
}

// What a person may be told when the others' doings and their own cross.
const expectedErrors = new Set(["illegal", "stalePrompt", "notYourTurn", "tableFull", "notFound", "nameTaken",
                                "openElsewhere", "notHost"]);

// ---- random numbers and time ----

function generator(seed) {
  let state = seed >>> 0;
  return () => {
    state = (state + 0x6D2B79F5) >>> 0;
    let t = state;
    t = Math.imul(t ^ (t >>> 15), t | 1);
    t ^= t + Math.imul(t ^ (t >>> 7), t | 61);
    return ((t ^ (t >>> 14)) >>> 0) / 4294967296;
  };
}

const sleep = (ms) => new Promise((resolve) => setTimeout(resolve, ms));

// One masked text frame, as a browser sends it.
export function textFrame(text) {
  const payload = Buffer.from(text, "utf8");
  const mask = Buffer.from([0x12, 0x34, 0x56, 0x78]);
  const header = payload.length < 126 ? Buffer.from([0x81, 0x80 | payload.length])
                                      : Buffer.from([0x81, 0x80 | 126, payload.length >> 8, payload.length & 0xff]);
  const masked = Buffer.from(payload.map((byte, index) => byte ^ mask[index % 4]));
  return Buffer.concat([header, mask, masked]);
}

// ---- the world ----

const portraits = { female: "woman-long-red-hair", male: "man-dark-hair-beard" };

function tableSettings(random) {
  const computer = (level, name, gender) => ({ kind: "computer", level, name, gender, avatar: portraits[gender] });
  return {
    seats: [{ kind: "open" }, { kind: "open" }, { kind: "open" },
            computer("first-card", "Dot", "female"), computer("low-risk", "Eli", "male"),
            computer("reckoner-alpha", "Fay", "female")],
    structure: random() < 0.5 ? "1-8-1" : "8-1-8",
    foreheadAndHidden: true,
    allOneCardForehead: random() < 0.3,
    botSpeed: 4,
    waitAfterRound: random() < 0.5,
    replacement: "reckoner-beta",
    turnTimer: 10000,
  };
}

class World {
  constructor({ url, random, quiet }) {
    this.url = url;
    this.random = random;
    this.quiet = quiet;
    this.failures = [];
    this.counts = {};
    this.stopped = false;
    this.disrupting = true;
  }

  count(name) {
    this.counts[name] = (this.counts[name] ?? 0) + 1;
  }

  fail(text) {
    if (this.failures.length < 50) {
      this.failures.push(text);
      console.error(`FAIL ${text}`);
    }
  }

  between(low, high) {
    return low + Math.floor(this.random() * (high - low + 1));
  }
}

// The people who sit at one table.
class Group {
  constructor(world, index) {
    this.world = world;
    this.index = index;
    this.code = null;
    this.state = "none";
    this.games = 0;
    this.latest = 0;
    this.finished = new Set();
    this.progress = Date.now();
    this.people = [];
    this.last = "-";
    this.round = "-";
  }

  connected() {
    return this.people.some((person) => person.connected);
  }

  // The table's code, and its state: a change of state is progress too.
  see(table) {
    this.code = table.code;
    if (table.state !== this.state) {
      this.state = table.state;
      this.touch();
    }
  }

  // Seconds since anything happened at the table, counted only while someone could have seen it.
  quietFor() {
    return this.connected() ? (Date.now() - this.progress) / 1000 : 0;
  }

  touch() {
    this.progress = Date.now();
  }

  // One line of what is known about the table, for the reports and for a failure.
  describe() {
    const people = this.people.map((person) => {
      const seat = person.mySeat();
      const where = person.table ? `${person.table.state}${seat === null ? "" : `/seat ${seat}`}` : "no table";
      return `${person.name} ${person.connected ? "on" : "off"} ${where}`;
    });
    return `table ${this.index} ${this.code ?? "-"}: ${this.state}, ${this.games} games finished, game ${this.latest}, round ${this.round}, ` +
           `last event ${this.last} ${Math.round((Date.now() - this.progress) / 1000)} s ago [${people.join("; ")}]`;
  }
}

class Person {
  constructor(group, name, gender, owner) {
    this.group = group;
    this.world = group.world;
    this.name = name;
    this.gender = gender;
    this.owner = owner;
    this.token = null;
    this.ws = null;
    this.connected = false;
    this.table = null;
    this.prompt = null;
    this.continued = null;
    this.busy = false;
    this.waiting = false;
    this.creating = false;
  }

  profile() {
    return { name: this.name, gender: this.gender, avatar: portraits[this.gender] };
  }

  send(message) {
    if (this.connected && this.ws.readyState === 1) {
      this.ws.send(JSON.stringify(message));
    }
  }

  // Opens a connection, says hello, and leaves the rest to what the server sends.
  open() {
    const ws = new WebSocket(this.world.url);
    this.ws = ws;
    ws.onopen = () => {
      this.connected = true;
      this.send({ type: "hello", protocol: 1, lang: "en", ...(this.token ? { token: this.token } : {}) });
    };
    ws.onmessage = (event) => {
      let message;
      try {
        message = JSON.parse(event.data);
      } catch {
        this.world.fail(`${this.name} was sent a message that is not JSON`);
        return;
      }
      this.receive(message);
    };
    ws.onclose = () => {
      if (this.ws === ws) {
        this.connected = false;
        this.prompt = null;
        this.waiting = false;
        this.creating = false;
      }
    };
    ws.onerror = () => {};
  }

  close() {
    this.ws?.close();
    this.connected = false;
    this.prompt = null;
    this.waiting = false;
    this.creating = false;
  }

  receive(message) {
    const { group, world } = this;
    switch (message.type) {
      case "welcome":
        this.token = message.token;
        return;
      case "sync":
        this.table = message.table;
        this.creating = false;
        if (message.table) {
          group.see(message.table);
        }
        checkAll(this, checkSnapshot(message.snapshot));
        this.prompt = null;
        if (message.prompt) {
          this.onPrompt(message.prompt);
        }
        if (message.held) {
          this.onHeld(message.held);
        }
        this.afterState();
        return;
      case "tableState":
        this.table = message.state === "closed" ? null : message;
        this.creating = false;
        group.see(message);
        this.afterState();
        return;
      case "game":
        group.touch();
        checkAll(this, checkSnapshot(message.snapshot));
        group.last = message.event.kind;
        if (message.snapshot?.round) {
          group.round = `${message.snapshot.round.index + 1}/${message.snapshot.round.count}`;
        }
        if (message.event.kind === "gameOver") {
          // The server sends no tableState for a game's end: the table is a lobby again from here.
          if (this.table) {
            this.table.state = "finished";
          }
          // A table made again numbers its games from 1, so a game is known by its table's code too.
          const key = `${this.table?.code ?? group.code}#${message.game}`;
          if (!group.finished.has(key)) {
            group.finished.add(key);
            group.latest = message.game;
            group.games++;
            group.state = "finished";
            world.count("games finished");
          }
          setTimeout(() => this.afterState(), 300);
        }
        return;
      case "prompt":
        this.onPrompt(message);
        return;
      case "promptClosed":
        if (this.prompt?.id === message.id) {
          this.prompt = null;
        }
        if (message.by === "computer") {
          world.count("turns made by the computer");
        }
        return;
      case "held":
        this.onHeld(message);
        return;
      case "error":
        world.count(`error ${message.code}`);
        if (!expectedErrors.has(message.code)) {
          world.fail(`${this.name} was sent error ${message.code} ${message.detail ?? ""}`);
        }
        if (message.code === "notFound" && this.owner && !this.table && !this.creating) {
          // The table is gone, and the owner makes another for everyone to join.
          world.count("tables made again");
          group.code = null;
          this.create();
        }
        return;
      default:
        world.fail(`${this.name} was sent a message of type ${message.type}`);
    }
  }

  mySeat() {
    const index = this.table?.seats?.findIndex((seat) => seat.you);
    return index === undefined || index < 0 ? null : index;
  }

  onPrompt(prompt) {
    checkAll(this, checkPrompt(prompt));
    this.prompt = prompt;
    const { world } = this;
    const roll = world.random();
    if (roll < 0.03) {
      // Idle: the turn timer will have the computer player move.
      world.count("turns left to the timer");
      return;
    }

    const answer = () => {
      if (this.prompt?.id !== prompt.id) {
        return;
      }
      this.send(this.valid(prompt));
    };
    const delay = world.between(0, 250);
    if (roll < 0.10) {
      world.count("illegal answers");
      this.send(prompt.kind === "bet" ? { type: "bet", prompt: prompt.id, value: 999 }
                                     : { type: "play", prompt: prompt.id, index: 99 });
    } else if (roll < 0.13) {
      world.count("stale answers");
      this.send({ ...this.valid(prompt), prompt: prompt.id + 1000 });
    }
    setTimeout(answer, delay);
  }

  valid(prompt) {
    if (prompt.kind === "bet") {
      const choices = [];
      for (let value = 0; value <= prompt.max; value++) {
        if (value !== prompt.forbidden) {
          choices.push(value);
        }
      }
      return { type: "bet", prompt: prompt.id, value: choices[this.world.between(0, choices.length - 1)] };
    }
    return { type: "play", prompt: prompt.id, index: prompt.legal[this.world.between(0, prompt.legal.length - 1)] };
  }

  onHeld(held) {
    const seat = this.mySeat();
    const awaited = held.waitingFor.length === 0 || (seat !== null && held.waitingFor.includes(seat));
    if (!awaited || this.continued === held.round || this.world.random() < 0.03) {
      return;
    }
    this.continued = held.round;
    setTimeout(() => this.send({ type: "continue", round: held.round }), this.world.between(0, 300));
  }

  // What a person does on seeing the table: the host starts games, anyone at no table joins one.
  afterState() {
    const { world } = this;
    if (!this.table) {
      this.wantTable();
      return;
    }

    const seat = this.mySeat();
    const hosting = seat !== null && this.table.host === seat;
    const between = this.table.state === "lobby" || this.table.state === "finished";
    if (!hosting || !between || this.busy) {
      return;
    }

    // People who left may be on their way back, so the host waits before giving their seats away. If
    // the game has not started by then, it is tried again.
    this.busy = true;
    setTimeout(() => {
      this.busy = false;
      const table = this.table;
      if (!table || !this.connected || world.stopped || !(table.state === "lobby" || table.state === "finished") ||
          this.mySeat() === null || table.host !== this.mySeat()) {
        return;
      }
      table.seats.forEach((open, index) => {
        if (open.kind === "open" && world.random() < 0.9) {
          world.count("seats given to computer players");
          this.send({ type: "setSeat", seat: index, kind: "computer", level: "first-card" });
        }
      });
      this.send({ type: "startGame" });
      setTimeout(() => this.afterState(), 5000);
    }, world.between(2500, 4500));
  }

  // At no table: tries to join the group's, and again every few seconds until it works.
  wantTable() {
    if (this.waiting || this.world.stopped) {
      return;
    }
    this.waiting = true;
    const attempt = () => {
      if (this.world.stopped || !this.connected || this.table) {
        this.waiting = false;
        return;
      }
      this.rejoin();
      setTimeout(attempt, this.world.between(2000, 5000));
    };
    setTimeout(attempt, this.world.between(200, 3000));
  }

  // Joins the group's table; the owner makes it when it has none yet.
  rejoin() {
    const { group } = this;
    if (!this.connected || this.table || this.world.stopped) {
      return;
    }
    if (group.code) {
      this.send({ type: "joinTable", code: group.code, profile: this.profile() });
    } else if (this.owner) {
      this.create();
    }
  }

  create() {
    if (this.creating) {
      return;
    }
    this.creating = true;
    this.send({ type: "createTable", settings: tableSettings(this.world.random), profile: this.profile() });
  }

  // A tab that goes silent without closing, as when a laptop's lid shuts. A socket of its own takes
  // the member over and then never answers a ping, so the server must notice the silence and close
  // it. Node's WebSocket answers pings by itself, so it cannot play this part.
  async freeze() {
    const { world } = this;
    if (!this.token) {
      return;
    }
    world.count("tabs frozen");
    const url = new URL(world.url);
    const socket = connect(Number(url.port), url.hostname);
    let closedByServer = false;
    let upgraded = false;
    let received = "";
    socket.on("error", () => {});
    socket.on("close", () => { closedByServer = true; });
    socket.on("data", (chunk) => {
      if (upgraded) {
        return;
      }
      received += chunk.toString("latin1");
      if (received.includes("\r\n\r\n")) {
        upgraded = true;
        socket.write(textFrame(JSON.stringify({ type: "hello", protocol: 1, lang: "en", token: this.token })));
      }
    });
    socket.on("connect", () => {
      socket.write(`GET ${url.pathname} HTTP/1.1\r\nHost: ${url.host}\r\nUpgrade: websocket\r\n` +
                   "Connection: Upgrade\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n" +
                   "Sec-WebSocket-Version: 13\r\n\r\n");
    });

    // The server's pong timeout, with room for its reaper's interval.
    const deadline = Date.now() + 15000;
    while (!closedByServer && Date.now() < deadline) {
      await sleep(250);
    }
    if (closedByServer) {
      world.count("frozen tabs closed by the server");
    } else if (upgraded) {
      world.fail(`${this.name}'s frozen tab was still open 15 s after it went silent`);
    }
    socket.destroy();
    if (!world.stopped) {
      this.open();
    }
  }

  // Closes the tab, or leaves, or takes the seat back, now and then, for as long as the test lasts.
  async disrupt() {
    const { world } = this;
    while (!world.stopped) {
      await sleep(world.between(3000, 12000));
      if (world.stopped || !world.disrupting || !this.connected) {
        continue;
      }
      const roll = world.random();
      if (roll < 0.1) {
        await this.freeze();
      } else if (roll < 0.6) {
        // Back within the grace period, or well after it.
        world.count("tabs closed");
        this.close();
        await sleep(world.random() < 0.5 ? world.between(100, 1500) : world.between(2500, 7000));
        this.open();
      } else if (roll < 0.8) {
        // The server answers with a sync of no table, and the person joins again a while after.
        world.count("people left");
        this.send({ type: "leaveTable" });
      } else {
        world.count("seats taken back");
        this.send({ type: "back" });
      }
    }
  }
}

function checkAll(person, problems) {
  for (const problem of problems) {
    person.world.fail(`${person.name} at table ${person.group.index}: ${problem}`);
  }
}

// ---- the server ----

async function freePort() {
  return new Promise((resolve, reject) => {
    const probe = createServer();
    probe.once("error", reject);
    probe.listen(0, "127.0.0.1", () => {
      const { port } = probe.address();
      probe.close(() => resolve(port));
    });
  });
}

async function startServer(program) {
  const directory = mkdtempSync(join(tmpdir(), "whist-chaos-"));
  const port = await freePort();
  writeFileSync(join(directory, "index.html"), "<!doctype html><title>chaos</title>\n");
  writeFileSync(join(directory, "config.json"), JSON.stringify({
    port, document_root: directory,
    ping_interval_ms: 1000, pong_timeout_ms: 5000, reap_interval_ms: 250,
    grace_period_ms: 2000, online_idle_ms: 30000, solo_idle_ms: 60000,
    instant_floor_ms: 10, log_level: "info",
  }));

  const child = spawn(program, ["--config", join(directory, "config.json")], { stdio: ["ignore", "pipe", "inherit"] });
  const tail = [];
  let exited = null;
  child.on("exit", (code, signal) => { exited = { code, signal }; });

  const listening = new Promise((resolve, reject) => {
    child.stdout.setEncoding("utf8");
    child.stdout.on("data", (text) => {
      for (const line of text.split("\n").filter(Boolean)) {
        tail.push(line);
        if (tail.length > 40) {
          tail.shift();
        }
        if (line.includes("listening on")) {
          resolve();
        }
      }
    });
    child.on("exit", () => reject(new Error("the server stopped before it listened")));
  });
  await listening;

  return {
    url: `ws://127.0.0.1:${port}/ws`,
    tail: () => tail.join("\n"),
    exited: () => exited,
    stop: async () => {
      child.kill("SIGTERM");
      await new Promise((resolve) => { child.on("exit", resolve); setTimeout(resolve, 5000); });
      child.kill("SIGKILL");
      rmSync(directory, { recursive: true, force: true });
    },
  };
}

// ---- the run ----

async function main() {
  const { values } = parseArgs({
    options: {
      server: { type: "string" }, url: { type: "string" }, minutes: { type: "string", default: "5" },
      tables: { type: "string", default: "4" }, seed: { type: "string" },
    },
  });
  if (!values.server === !values.url) {
    console.error("give --server <program> or --url <ws://host:port/ws>, one of them");
    return 2;
  }

  const minutes = Number(values.minutes);
  const seed = values.seed === undefined ? Date.now() % 100000 : Number(values.seed);
  const server = values.server ? await startServer(values.server) : null;
  process.once("SIGINT", async () => {
    await server?.stop();
    process.exit(130);
  });
  const world = new World({ url: server ? server.url : values.url, random: generator(seed) });
  console.log(`seed ${seed}, ${values.tables} tables for ${minutes} minutes against ${world.url}`);

  const groups = [];
  const people = [];
  for (let index = 0 ; index < Number(values.tables) ; index++) {
    const group = new Group(world, index);
    group.people = [new Person(group, "Ana", "female", true), new Person(group, "Bea", "female", false),
                    new Person(group, "Cezar", "male", false)];
    groups.push(group);
    people.push(...group.people);
  }

  // The owner makes the table, then the others join it.
  for (const group of groups) {
    group.people[0].open();
    setTimeout(() => group.people[1].open(), 800);
    setTimeout(() => group.people[2].open(), 1200);
  }
  await sleep(1500);
  for (const group of groups) {
    group.people[0].create();
  }
  for (const person of people) {
    void person.disrupt();
  }

  // A table that has made no progress for longer than its longest legitimate wait is stalled: a turn
  // (10 s), the grace period, and a hold after a round (30 s), with room to spare.
  const stallLimit = 120;
  const watchdog = setInterval(() => {
    for (const group of groups) {
      if (group.quietFor() > stallLimit && group.state !== "closed") {
        world.fail(`no progress for ${Math.round(group.quietFor())} s at ${group.describe()}`);
        group.touch();
      }
    }
    if (server?.exited()) {
      world.fail(`the server stopped: ${JSON.stringify(server.exited())}`);
    }
  }, 5000);

  const report = setInterval(() => {
    console.log(`${new Date().toISOString()} ${JSON.stringify(world.counts)}`);
    for (const group of groups) {
      console.log(`  ${group.describe()}`);
    }
  }, 30000);

  await sleep(minutes * 60000);

  // Everyone comes back and stays. Tables go on with game after game, so each one must finish a game
  // from here: the one that is on, or one its host starts.
  world.disrupting = false;
  const finishedAtStop = groups.map((group) => group.games);
  const unfinished = () => groups.filter((group, index) => group.games <= finishedAtStop[index]);
  const settleBy = Date.now() + 300000;
  while (Date.now() < settleBy && unfinished().length > 0) {
    for (const person of people) {
      if (!person.connected) {
        person.open();
      }
    }
    await sleep(1000);
  }
  for (const group of unfinished()) {
    world.fail(`no game finished in the 5 minutes after the last disruption: ${group.describe()}`);
  }
  for (const group of groups) {
    if (group.games === 0) {
      world.fail(`no game finished at all: ${group.describe()}`);
    }
  }

  world.stopped = true;
  clearInterval(watchdog);
  clearInterval(report);
  for (const person of people) {
    person.close();
  }
  console.log(`games per table: ${groups.map((group) => group.games).join(", ")}`);
  console.log(JSON.stringify(world.counts, null, 2));
  if (server) {
    if (world.failures.length > 0) {
      console.error(`the server's last lines:\n${server.tail()}`);
    }
    await server.stop();
  }
  console.log(world.failures.length === 0 ? "PASS" : `FAIL: ${world.failures.length} problems, the first above`);
  return world.failures.length === 0 ? 0 : 1;
}

if (process.argv[1] && import.meta.url === pathToFileURL(process.argv[1]).href) {
  main().then((status) => process.exit(status), (error) => {
    console.error(error);
    process.exit(1);
  });
}
