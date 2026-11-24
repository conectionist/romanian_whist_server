#!/usr/bin/env node
// Plays many tables against a server, in steps, to find how many it carries: how long an answer takes
// to echo, and how much of the machine the server uses. Node 22 or later, no packages.

import { execFile, execFileSync, spawn } from "node:child_process";
import { closeSync, existsSync, openSync, readFileSync, readSync, statSync, writeFileSync } from "node:fs";
import os from "node:os";
import { pathToFileURL } from "node:url";
import { parseArgs } from "node:util";
import { protocol, waitForPage } from "../smoke/smoke.mjs";

// ---- what is measured ----

export function percentile(sorted, p) {
  if (sorted.length === 0) {
    return null;
  }
  return sorted[Math.min(sorted.length - 1, Math.max(0, Math.ceil((p / 100) * sorted.length) - 1))];
}

export function latencyOf(samples) {
  const sorted = [...samples].sort((a, b) => a - b);
  return {
    count: sorted.length,
    p50: percentile(sorted, 50),
    p95: percentile(sorted, 95),
    p99: percentile(sorted, 99),
    max: sorted.length > 0 ? sorted[sorted.length - 1] : null,
  };
}

// Which memory a reading is, since the two do not compare: the status line says the most the server
// has held since it started, and /proc and docker stats say what it holds now.
export const MEMORY_KINDS = {
  peak: { column: "peak MB", note: "peak resident memory since the server started, from its status line" },
  current: { column: "cur MB", note: "resident memory when read, from /proc or docker stats" },
};

// A status line of the server's log: "status connections=3 tables_playing=1 ...", with the words
// that follow it ignored. Null for a line that is not one.
export function parseStatusLine(text) {
  const at = text.indexOf("status ");
  if (at < 0) {
    return null;
  }
  const pairs = {};
  for (const word of text.slice(at + "status ".length).split(/\s+/)) {
    const equals = word.indexOf("=");
    if (equals > 0 && /^-?\d+(\.\d+)?$/.test(word.slice(equals + 1))) {
      pairs[word.slice(0, equals)] = Number(word.slice(equals + 1));
    }
  }
  if (!("connections" in pairs) || !("cpu_percent" in pairs)) {
    return null;
  }
  return {
    connections: pairs.connections,
    gameThreads: pairs.game_threads ?? null,
    cpuCores: pairs.cpu_percent / 100,
    memoryMb: pairs.peak_memory_mb ?? null,
    memoryKind: "peak_memory_mb" in pairs ? "peak" : null,
  };
}

// What `docker stats` prints for --format "{{.CPUPerc}}|{{.MemUsage}}": "12.34%|45.6MiB / 1.9GiB".
export function parseDockerStats(text) {
  const match = /^\s*([\d.]+)%\s*\|\s*([\d.]+)\s*([KMG]i?B)\b/.exec(text);
  if (!match) {
    return null;
  }
  const unit = { KB: 1 / 1000, KiB: 1 / 1024, MB: 1, MiB: 1.048576, GB: 1000, GiB: 1073.741824 }[match[3]];
  return { cpuCores: Number(match[1]) / 100, memoryMb: Math.round(Number(match[2]) * unit), memoryKind: "current" };
}

// A step is good when it is quick enough, leaves the machine enough room, and went without an error
// from the server, a connection that closed, or a table that would not start.
export function suggestCap(steps, { p99Ms = 250, cpuShare = 0.7, roundTo = 10 } = {}) {
  let cap = null;
  for (const step of steps) {
    if (step.cpuShare === null || step.cpuShare === undefined) {
      return { cap, why: "the server's CPU was not read, so no step can be judged" };
    }
    if (step.latency.p99 === null || step.latency.p99 >= p99Ms || step.cpuShare >= cpuShare
      || (step.errors ?? 0) > 0 || (step.drops ?? 0) > 0 || (step.failedToStart ?? 0) > 0) {
      break;
    }
    cap = step.tables;
  }
  if (cap === null) {
    return {
      cap: null,
      why: `not even the first step had p99 under ${p99Ms} ms, the CPU under ${Math.round(cpuShare * 100)}%, and no errors, drops or tables that failed to start`,
    };
  }
  const rounded = Math.floor(cap / roundTo) * roundTo;
  return rounded > 0
    ? { cap: rounded, why: null }
    : { cap: null, why: `the largest good step, ${cap} tables, is under ${roundTo}` };
}

// What each table cost: the memory above the idle server's, shared by the tables of the step. Null
// where either reading is missing, or they are not the same kind of memory.
export function memoryPerTable(step, baseline) {
  if (!baseline || step.memoryMb === null || step.memoryMb === undefined || step.memoryKind !== baseline.memoryKind
    || !(step.tables > 0)) {
    return null;
  }
  return Math.max(0, step.memoryMb - baseline.memoryMb) / step.tables;
}

const pad = (value, width) => String(value).padStart(width);
const ms = (value) => (value === null ? "-" : `${Math.round(value)}`);

// The name of the memory the steps read, or "mem MB" where they read none or two kinds.
function memoryColumn(steps) {
  const kinds = new Set(steps.map((step) => step.memoryKind).filter(Boolean));
  return kinds.size === 1 ? MEMORY_KINDS[[...kinds][0]].column : "mem MB";
}

export function summaryTable(kindName, steps, { cores, hold, baseline = null }) {
  const lines = [
    `${kindName}: ${hold} s held at each step, on ${cores} core${cores === 1 ? "" : "s"}`,
    `tables  conns  threads  answers   p50   p95   p99   max   cpu%   cores  ${memoryColumn(steps)}  MB/tbl  games  errors  drops`,
  ];
  for (const step of steps) {
    const cpu = step.cpuShare === null ? "-" : (step.cpuShare * 100).toFixed(0);
    const used = step.cpuCores === null ? "-" : step.cpuCores.toFixed(2);
    lines.push([
      pad(step.tables, 6), pad(step.connections ?? "-", 6), pad(step.gameThreads ?? "-", 8), pad(step.latency.count, 8),
      pad(ms(step.latency.p50), 5), pad(ms(step.latency.p95), 5), pad(ms(step.latency.p99), 5), pad(ms(step.latency.max), 5),
      pad(cpu, 6), pad(used, 7), pad(step.memoryMb ?? "-", 7), pad(step.memoryPerTableMb === null || step.memoryPerTableMb === undefined ? "-" : step.memoryPerTableMb.toFixed(2), 6),
      pad(step.gamesFinished, 6), pad(step.errors, 7), pad(step.drops, 6),
    ].join(" "));
  }
  const kinds = [...new Set(steps.map((step) => step.memoryKind).filter(Boolean))];
  for (const kind of kinds) {
    lines.push(`${kinds.length === 1 ? "memory" : MEMORY_KINDS[kind].column}: ${MEMORY_KINDS[kind].note}`);
  }
  if (baseline && steps.some((step) => step.memoryPerTableMb !== null && step.memoryPerTableMb !== undefined)) {
    lines.push(`MB/tbl: the memory above the idle server's ${baseline.memoryMb} MB, shared by the tables of the step`);
  }
  return lines.join("\n");
}

// What the tables of the biggest step that has a figure say about a cap on tables.
export function memoryForCap(steps, maxTables) {
  const known = steps.filter((step) => step.memoryPerTableMb !== null && step.memoryPerTableMb !== undefined);
  if (known.length === 0) {
    return null;
  }
  const last = known[known.length - 1];
  return { tables: last.tables, perTableMb: last.memoryPerTableMb, capMb: last.memoryPerTableMb * maxTables, maxTables };
}

// ---- what a scripted person says ----

const PEOPLE = [
  ["Ana", "female", "woman-long-red-hair"], ["Bogdan", "male", "man-dark-hair-beard"],
  ["Cora", "female", "woman-long-red-hair"], ["Dan", "male", "man-dark-hair-beard"],
  ["Ema", "female", "woman-long-red-hair"], ["Filip", "male", "man-dark-hair-beard"],
];

const profileOf = (index) => ({ name: PEOPLE[index][0], gender: PEOPLE[index][1], avatar: PEOPLE[index][2] });

export function soloGame(botSpeed) {
  return {
    type: "createSolo",
    settings: {
      structure: "1-8-1",
      botSpeed,
      seats: PEOPLE.map((person, index) => (index === 0
        ? { kind: "human", ...profileOf(0) }
        : { kind: "computer", level: "reckoner-gamma", ...profileOf(index) })),
    },
  };
}

export function onlineTable() {
  return {
    type: "createTable",
    profile: profileOf(0),
    settings: { structure: "1-8-1", waitAfterRound: false, seats: PEOPLE.map(() => ({ kind: "open" })) },
  };
}

// The first bid that is allowed, and the first card that is legal: a person who answers at once.
export function chooseAnswer(prompt) {
  if (prompt.kind === "bet") {
    for (let value = 0; value <= prompt.max; value++) {
      if (value !== prompt.forbidden) {
        return { type: "bet", prompt: prompt.id, value };
      }
    }
  }
  return { type: "play", prompt: prompt.id, index: prompt.legal[0] };
}

// The Instant cap is small, so its suggestion is not rounded to 10.
const KINDS = {
  solo: { name: "solo tables at normal speed", botSpeed: 2, online: false, cap: "max_running_games", games: "running games", roundTo: 10 },
  instant: { name: "solo tables at Instant", botSpeed: 4, online: false, cap: "max_instant_games", games: "Instant games", roundTo: 1 },
  online: { name: "online tables of six people", botSpeed: null, online: true, cap: "max_running_games", games: "running games", roundTo: 10 },
};

const sleep = (time) => new Promise((resolve) => setTimeout(resolve, time));

// ---- the people ----

class Person {
  constructor(run, index, onMessage) {
    this.run = run;
    this.profile = profileOf(index);
    this.onMessage = onMessage;
    this.seat = null;
    this.sentAt = null;
    this.ready = false;
    this.closing = false;
  }

  connect() {
    return new Promise((resolve, reject) => {
      const timer = setTimeout(() => reject(new Error("no welcome and sync in 20 s")), 20000);
      this.socket = new WebSocket(this.run.url);
      this.socket.addEventListener("open", () => this.send({ type: "hello", protocol, lang: "en" }));
      this.socket.addEventListener("message", (event) => {
        const message = JSON.parse(event.data);
        if (message.type === "sync" && !this.ready) {
          this.ready = true;
          clearTimeout(timer);
          resolve();
        }
        this.receive(message);
      });
      this.socket.addEventListener("error", () => {
        clearTimeout(timer);
        reject(new Error("the connection failed"));
      });
      this.socket.addEventListener("close", (event) => {
        clearTimeout(timer);
        if (this.ready && !this.closing) {
          this.run.drops += 1;
          this.run.note(`a connection closed with code ${event.code}`);
        }
        reject(new Error(`the connection closed with code ${event.code}`));
      });
    });
  }

  send(message) {
    if (this.socket.readyState === WebSocket.OPEN) {
      this.socket.send(JSON.stringify(message));
    }
  }

  close() {
    this.closing = true;
    this.socket?.close();
  }

  receive(message) {
    switch (message.type) {
      case "prompt":
        this.sentAt = performance.now();
        this.send(chooseAnswer(message));
        break;
      case "game": {
        const { kind, seat } = message.event;
        if ((kind === "betPlaced" || kind === "cardPlayed") && seat === this.seat && this.sentAt !== null) {
          this.run.record(performance.now() - this.sentAt);
          this.sentAt = null;
        }
        break;
      }
      case "tableState":
      case "sync": {
        const table = message.type === "sync" ? message.table : message;
        const seat = table?.seats?.findIndex((s) => s.you);
        if (seat !== undefined && seat >= 0) {
          this.seat = seat;
        }
        break;
      }
      case "error":
        this.run.error(message.code);
        break;
      default:
        break;
    }
    this.onMessage(this, message);
  }
}

// One person against five computer players, game after game.
class SoloTable {
  constructor(run, index) {
    this.run = run;
    this.index = index;
    this.person = new Person(run, 0, (person, message) => this.hear(message));
  }

  async start() {
    await this.person.connect();
    this.person.send(soloGame(KINDS[this.run.kind].botSpeed));
  }

  hear(message) {
    if (message.type === "game" && message.event.kind === "gameOver") {
      this.run.gamesFinished += 1;
      this.person.send({ type: "leaveTable" });
      setTimeout(() => this.person.send(soloGame(KINDS[this.run.kind].botSpeed)), 50);
    }
  }

  close() {
    this.person.close();
  }
}

// Six people at one table, game after game: the first makes it, the rest join, and the first starts
// each game.
class OnlineTable {
  constructor(run, index) {
    this.run = run;
    this.index = index;
    this.code = null;
    this.waiting = true;
    this.people = PEOPLE.map((p, i) => new Person(run, i, (person, message) => this.hear(person, message)));
  }

  async start() {
    await Promise.all(this.people.map((person) => person.connect()));
    this.people[0].send(onlineTable());
  }

  hear(person, message) {
    const host = this.people[0];
    if (person === host && message.type === "sync" && message.table && this.code === null) {
      this.code = message.table.code;
      this.people.slice(1).forEach((other, i) => other.send({ type: "joinTable", code: this.code, profile: profileOf(i + 1) }));
    }
    if (person === host && message.type === "tableState" && message.state === "lobby" && this.waiting
      && message.seats.every((seat) => seat.kind === "human")) {
      this.waiting = false;
      host.send({ type: "startGame" });
    }
    if (person === host && message.type === "game" && message.event.kind === "gameOver") {
      this.run.gamesFinished += 1;
      setTimeout(() => host.send({ type: "startGame" }), 100);
    }
  }

  close() {
    this.people.forEach((person) => person.close());
  }
}

// ---- what the server says about itself ----

class StatusSamples {
  constructor() {
    this.samples = [];
  }

  add(sample) {
    this.samples.push({ at: Date.now(), ...sample });
  }

  between(from, to) {
    return this.samples.filter((sample) => sample.at >= from && sample.at <= to);
  }

  latest() {
    return this.samples.length === 0 ? null : this.samples[this.samples.length - 1];
  }

  // The averages over a stretch of time, and the last of the rest.
  over(from, to) {
    const inside = this.between(from, to);
    if (inside.length === 0) {
      return { cpuCores: null, memoryMb: null, memoryKind: null, gameThreads: null, connections: null };
    }
    const last = inside[inside.length - 1];
    return {
      cpuCores: inside.reduce((sum, sample) => sum + sample.cpuCores, 0) / inside.length,
      memoryMb: last.memoryMb,
      memoryKind: last.memoryMb === null || last.memoryMb === undefined ? null : (last.memoryKind ?? null),
      gameThreads: last.gameThreads ?? null,
      connections: last.connections ?? null,
    };
  }
}

// Lines of the server's log as they come, which may split anywhere.
class LineReader {
  constructor(onLine) {
    this.onLine = onLine;
    this.rest = "";
  }

  feed(text) {
    this.rest += text;
    const lines = this.rest.split("\n");
    this.rest = lines.pop();
    lines.forEach((line) => this.onLine(line));
  }
}

function watchStatusLines(samples) {
  return new LineReader((line) => {
    const sample = parseStatusLine(line);
    if (sample) {
      samples.add(sample);
    }
  });
}

// A file that the server's log is written to, read from where it ends now.
function followFile(path, reader) {
  let position = existsSync(path) ? statSync(path).size : 0;
  const timer = setInterval(() => {
    if (!existsSync(path)) {
      return;
    }
    const size = statSync(path).size;
    if (size < position) {
      position = 0;
    }
    if (size > position) {
      const descriptor = openSync(path, "r");
      const buffer = Buffer.alloc(size - position);
      readSync(descriptor, buffer, 0, buffer.length, position);
      closeSync(descriptor);
      position = size;
      reader.feed(buffer.toString("utf8"));
    }
  }, 500);
  return () => clearInterval(timer);
}

// A process on this machine, read from /proc once a second.
function followProcess(pid, samples) {
  const ticks = Number(execFileSync("getconf", ["CLK_TCK"]).toString().trim()) || 100;
  const cpuTime = () => {
    const fields = readFileSync(`/proc/${pid}/stat`, "utf8").replace(/^.*\) /, "").split(" ");
    return (Number(fields[11]) + Number(fields[12])) / ticks;
  };
  const memory = () => Number(/VmRSS:\s+(\d+)/.exec(readFileSync(`/proc/${pid}/status`, "utf8"))[1]) / 1024;
  let before = { at: Date.now(), cpu: cpuTime() };
  const timer = setInterval(() => {
    try {
      const now = { at: Date.now(), cpu: cpuTime() };
      samples.add({ cpuCores: (now.cpu - before.cpu) / ((now.at - before.at) / 1000), memoryMb: Math.round(memory()), memoryKind: "current" });
      before = now;
    } catch {
      // The process is gone; the run says so by its answers stopping.
    }
  }, 1000);
  return () => clearInterval(timer);
}

// A container, read with `docker stats`, which takes a second or two for each answer.
function followContainer(name, samples) {
  let stopped = false;
  const ask = () => {
    if (stopped) {
      return;
    }
    execFile("docker", ["stats", "--no-stream", "--format", "{{.CPUPerc}}|{{.MemUsage}}", name], (error, out) => {
      const sample = error ? null : parseDockerStats(out);
      if (sample) {
        samples.add(sample);
      }
      setTimeout(ask, 1000);
    });
  };
  ask();
  return () => { stopped = true; };
}

async function startServer(program, configPath, samples) {
  const child = spawn(program, ["--config", configPath], { stdio: ["ignore", "pipe", "inherit"] });
  const reader = watchStatusLines(samples);
  let exited = null;
  child.on("exit", (code, signal) => { exited = { code, signal }; });
  await new Promise((resolve, reject) => {
    child.stdout.setEncoding("utf8");
    child.stdout.on("data", (text) => {
      reader.feed(text);
      if (text.includes("listening on")) {
        resolve();
      }
    });
    child.on("exit", () => reject(new Error("the server stopped before it listened")));
  });
  return {
    exited: () => exited,
    stop: async () => {
      child.kill("SIGTERM");
      await new Promise((resolve) => { child.on("exit", resolve); setTimeout(resolve, 30000); });
      child.kill("SIGKILL");
    },
  };
}

// ---- the run ----

class Run {
  constructor(url, kind) {
    this.url = url;
    this.kind = kind;
    this.tables = [];
    this.latencies = [];
    this.measuring = false;
    this.gamesFinished = 0;
    this.errors = {};
    this.drops = 0;
    this.failedToStart = 0;
    this.notes = new Set();
  }

  record(latency) {
    if (this.measuring) {
      this.latencies.push(latency);
    }
  }

  error(code) {
    this.errors[code] = (this.errors[code] ?? 0) + 1;
  }

  note(text) {
    if (!this.notes.has(text) && this.notes.size < 20) {
      this.notes.add(text);
      console.log(`note: ${text}`);
    }
  }

  // Tables are made a few at a time, so that a step does not start every game at the same moment.
  async growTo(target) {
    while (this.tables.length < target) {
      const batch = [];
      for (let i = 0 ; i < 5 && this.tables.length + batch.length < target ; i++) {
        const index = this.tables.length + batch.length;
        const table = KINDS[this.kind].online ? new OnlineTable(this, index) : new SoloTable(this, index);
        batch.push(table.start().then(() => table, (error) => {
          table.close();
          this.failedToStart += 1;
          this.note(`a table did not start: ${error.message}`);
          return null;
        }));
      }
      const started = await Promise.all(batch);
      const made = started.filter(Boolean);
      this.tables.push(...made);
      if (made.length === 0) {
        throw new Error("no table could be started");
      }
      await sleep(200);
    }
  }

  close() {
    this.tables.forEach((table) => table.close());
  }

  errorCount() {
    return Object.values(this.errors).reduce((sum, count) => sum + count, 0);
  }
}

// The idle server's memory: the first reading that has some, waited for up to `waitMs`, which has to cover the
// status log's interval, or no reading comes and there is no memory per table.
export async function idleMemory(samples, waitMs, { pollMs = 250, now = Date.now, wait = sleep } = {}) {
  const until = now() + waitMs;
  for (;;) {
    const sample = samples.latest();
    if (sample && sample.memoryMb !== null && sample.memoryMb !== undefined && sample.memoryKind) {
      return { memoryMb: sample.memoryMb, memoryKind: sample.memoryKind };
    }
    if (now() >= until) {
      return null;
    }
    await wait(pollMs);
  }
}

function usage() {
  return `usage: node tools/load/load.mjs --kind solo|instant|online [--url http://127.0.0.1:8080]
         [--steps 10,20,40,60,80,100] [--hold 180] [--settle 15] [--cores N]
         [--server <program> --config <file> | --status-log <file> | --pid <n> | --container <name>]
         [--p99 250] [--cpu 70] [--stop-p99 1000] [--stop-cpu 95] [--max-tables 400] [--baseline-wait 12] [--json <file>]`;
}

async function main() {
  const { values } = parseArgs({
    options: {
      kind: { type: "string" }, url: { type: "string" }, steps: { type: "string", default: "10,20,40,60,80,100" },
      hold: { type: "string", default: "180" }, settle: { type: "string", default: "15" }, cores: { type: "string" },
      server: { type: "string" }, config: { type: "string" }, "status-log": { type: "string" },
      pid: { type: "string" }, container: { type: "string" }, p99: { type: "string", default: "250" },
      cpu: { type: "string", default: "70" }, "stop-p99": { type: "string", default: "1000" },
      "stop-cpu": { type: "string", default: "95" }, "max-tables": { type: "string", default: "400" }, "baseline-wait": { type: "string", default: "12" }, json: { type: "string" },
    },
  });
  if (!(values.kind in KINDS) || (values.server && !values.config)) {
    console.error(usage());
    return 2;
  }
  const steps = values.steps.split(",").map(Number);
  const hold = Number(values.hold);
  const settle = Number(values.settle);
  const cores = values.cores ? Number(values.cores) : os.availableParallelism();
  if (steps.some((step) => !(step > 0)) || !(hold > 0) || !(settle >= 0) || !(cores > 0)) {
    console.error(usage());
    return 2;
  }
  const maxTables = Number(values["max-tables"]);
  const baselineWait = Number(values["baseline-wait"]);
  if (!(maxTables > 0) || !(baselineWait > 0)) {
    console.error(usage());
    return 2;
  }
  const limits = { p99Ms: Number(values.p99), cpuShare: Number(values.cpu) / 100 };
  const stopAt = { p99Ms: Number(values["stop-p99"]), cpuShare: Number(values["stop-cpu"]) / 100 };

  const samples = new StatusSamples();
  const stoppers = [];
  let server = null;
  let url = values.url;
  if (values.server) {
    server = await startServer(values.server, values.config, samples);
    url ??= `http://127.0.0.1:${JSON.parse(readFileSync(values.config, "utf8")).port ?? 8080}`;
  }
  url ??= "http://127.0.0.1:8080";
  if (values["status-log"]) {
    stoppers.push(followFile(values["status-log"], watchStatusLines(samples)));
  }
  if (values.pid) {
    stoppers.push(followProcess(Number(values.pid), samples));
  }
  if (values.container) {
    stoppers.push(followContainer(values.container, samples));
  }
  const watching = Boolean(server || values["status-log"] || values.pid || values.container);

  const run = new Run(url.replace(/^http/, "ws").replace(/\/?$/, "/ws"), values.kind);
  const results = [];
  let baseline = null;
  let failure = null;

  // Ctrl-C leaves nothing behind: the connections close, and a server this script started stops.
  process.once("SIGINT", async () => {
    run.close();
    stoppers.forEach((stop) => stop());
    await server?.stop();
    process.exit(130);
  });
  try {
    await waitForPage(url);
    const version = await (await fetch(new URL("/version", url))).json();
    console.log(`server ${version.version}, commit ${version.commit}, protocol ${version.protocol}; ${KINDS[values.kind].name}`);
    if (version.protocol !== protocol) {
      throw new Error(`the server speaks protocol ${version.protocol} and this script ${protocol}`);
    }

    if (watching) {
      baseline = await idleMemory(samples, baselineWait * 1000);
      if (baseline === null) {
        run.note(`no reading of the idle server's memory in ${baselineWait} s, so there is no memory per table: --baseline-wait must be longer than the status log's interval`);
      }
    }

    for (const target of steps) {
      const failedBefore = run.failedToStart;
      await run.growTo(target);
      await sleep(settle * 1000);

      const before = { games: run.gamesFinished, errors: run.errorCount(), drops: run.drops };
      run.latencies = [];
      run.measuring = true;
      const from = Date.now();
      await sleep(hold * 1000);
      const to = Date.now();
      run.measuring = false;

      const reading = watching ? samples.over(from, to) : { cpuCores: null, memoryMb: null, memoryKind: null, gameThreads: null, connections: null };
      const step = {
        tables: run.tables.length,
        latency: latencyOf(run.latencies),
        cpuCores: reading.cpuCores,
        cpuShare: reading.cpuCores === null ? null : reading.cpuCores / cores,
        memoryMb: reading.memoryMb,
        memoryKind: reading.memoryKind,
        gameThreads: reading.gameThreads,
        connections: reading.connections,
        gamesFinished: run.gamesFinished - before.games,
        errors: run.errorCount() - before.errors,
        drops: run.drops - before.drops,
        failedToStart: run.failedToStart - failedBefore,
      };
      step.memoryPerTableMb = memoryPerTable(step, baseline);
      results.push(step);
      console.log(`${pad(step.tables, 4)} tables: p50 ${ms(step.latency.p50)} ms, p95 ${ms(step.latency.p95)} ms, p99 ${ms(step.latency.p99)} ms`
        + `, cpu ${step.cpuShare === null ? "-" : `${(step.cpuShare * 100).toFixed(0)}%`}, ${step.errors} errors, ${step.drops} drops`
        + (step.failedToStart > 0 ? `, ${step.failedToStart} tables that failed to start` : "")
        + (step.memoryPerTableMb === null ? "" : `, ${step.memoryPerTableMb.toFixed(2)} MB a table`));

      if (step.latency.count === 0) {
        run.note("no answer was timed at this step: the games are not moving");
        break;
      }
      if (step.latency.p99 >= stopAt.p99Ms || (step.cpuShare !== null && step.cpuShare >= stopAt.cpuShare) || run.failedToStart > 0) {
        console.log("stopping the ramp: the server is past what a step further would tell");
        break;
      }
    }
  } catch (error) {
    failure = error;
  } finally {
    run.close();
    stoppers.forEach((stop) => stop());
    await server?.stop();
  }

  if (failure) {
    console.error(`FAIL ${failure.message}`);
  }
  if (results.length > 0) {
    console.log(`\n${summaryTable(KINDS[values.kind].name, results, { cores, hold, baseline })}`);
    const memory = memoryForCap(results, maxTables);
    if (memory) {
      const note = MEMORY_KINDS[baseline.memoryKind].note;
      console.log(`\nMemory: ${memory.perTableMb.toFixed(2)} MB a table at ${memory.tables} tables, so ${memory.maxTables} tables (\`max_tables\`) would take about ${Math.round(memory.capMb)} MB more than an idle server. It is ${note}.`);
    }
    const kind = KINDS[values.kind];
    const { cap, why } = suggestCap(results, { ...limits, roundTo: kind.roundTo });
    console.log(cap === null
      ? `\nNo cap to suggest: ${why}.`
      : `\nSuggested cap: ${cap} ${kind.games} ("${kind.cap}": ${cap}), the largest step with p99 under ${limits.p99Ms} ms and the server under ${Math.round(limits.cpuShare * 100)}% of ${cores} cores.`
        + (values.kind === "instant" ? " That is with nothing else running: leave room for the other games." : ""));
    if (Object.keys(run.errors).length > 0) {
      console.log(`The server sent errors: ${JSON.stringify(run.errors)}. Its limits may be in the way: it should run with deploy/load-config.json.`);
    }
    if (values.json) {
      writeFileSync(values.json, `${JSON.stringify({ kind: values.kind, cores, hold, baseline, steps: results, errors: run.errors, cap, memory }, null, 2)}\n`);
    }
  }
  return failure || run.errorCount() > 0 || run.drops > 0 ? 1 : 0;
}

if (process.argv[1] && import.meta.url === pathToFileURL(process.argv[1]).href) {
  main().then((code) => process.exit(code), (error) => {
    console.error(`FAIL ${error.message}`);
    process.exit(1);
  });
}
