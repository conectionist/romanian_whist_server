// The load test's arithmetic and its scripted people, with no server: node --test tools/load/load.test.mjs
import assert from "node:assert/strict";
import test from "node:test";

import {
  chooseAnswer, latencyOf, onlineTable, parseDockerStats, parseStatusLine, percentile, soloGame, suggestCap, summaryTable,
} from "./load.mjs";

test("a percentile is the value that this share of the samples do not exceed", () => {
  const hundred = Array.from({ length: 100 }, (_, i) => i + 1);
  assert.equal(percentile(hundred, 50), 50);
  assert.equal(percentile(hundred, 95), 95);
  assert.equal(percentile(hundred, 99), 99);
  assert.equal(percentile([7], 99), 7);
  assert.equal(percentile([1, 2, 3], 50), 2);
  assert.equal(percentile([], 50), null);
});

test("latency is summed up from samples in any order", () => {
  const latency = latencyOf([30, 10, 20, 1000, 40]);
  assert.deepEqual(latency, { count: 5, p50: 30, p95: 1000, p99: 1000, max: 1000 });
  assert.deepEqual(latencyOf([]), { count: 0, p50: null, p95: null, p99: null, max: null });
});

test("a status line of the server's log is read, whatever comes before and after it", () => {
  const line = "20261117 14:19:40.774784 UTC 5490 INFO  status connections=12 tables_lobby=1 tables_playing=2 tables_finished=0 "
    + "game_threads=2 stand_in_moves=17 cpu_percent=71.5 cpu_seconds=123.4 peak_memory_mb=45 - Server.cpp:107";
  assert.deepEqual(parseStatusLine(line), { connections: 12, gameThreads: 2, cpuCores: 0.715, memoryMb: 45, memoryKind: "peak" });
  assert.deepEqual(parseStatusLine("server-1  | status connections=0 cpu_percent=0.1 game_threads=0"),
    { connections: 0, gameThreads: 0, cpuCores: 0.001, memoryMb: null, memoryKind: null });
  assert.equal(parseStatusLine("20261117 INFO  listening on 127.0.0.1:8080"), null);
  assert.equal(parseStatusLine("status connections=1"), null);
});

test("docker stats is read in cores and megabytes, and its memory is the current one", () => {
  assert.deepEqual(parseDockerStats("12.34%|45.6MiB / 1.9GiB"), { cpuCores: 0.1234, memoryMb: 48, memoryKind: "current" });
  assert.deepEqual(parseDockerStats("151.00%|1.2GiB / 1.9GiB"), { cpuCores: 1.51, memoryMb: 1288, memoryKind: "current" });
  assert.equal(parseDockerStats("--"), null);
});

const step = (tables, p99, cpuShare) => ({ tables, latency: { count: 100, p99 }, cpuShare });

test("the cap is the largest step that is quick and leaves room, rounded down to ten", () => {
  assert.deepEqual(suggestCap([step(10, 20, 0.1), step(20, 40, 0.3), step(40, 90, 0.6)]), { cap: 40, why: null });
  assert.deepEqual(suggestCap([step(10, 20, 0.1), step(25, 40, 0.3)]), { cap: 20, why: null });

  // The first step that is too slow, or too busy, ends it, though a later one might look better.
  assert.equal(suggestCap([step(10, 20, 0.1), step(20, 250, 0.3), step(40, 90, 0.5)]).cap, 10);
  assert.equal(suggestCap([step(10, 20, 0.1), step(20, 40, 0.7), step(40, 90, 0.5)]).cap, 10);
  assert.equal(suggestCap([step(10, 20, 0.1), step(20, 249, 0.69)]).cap, 20);
});

test("with no step good enough, or no CPU reading, there is no cap and the reason is given", () => {
  assert.equal(suggestCap([step(10, 400, 0.1)]).cap, null);
  assert.match(suggestCap([step(10, 400, 0.1)]).why, /first step/);
  assert.equal(suggestCap([step(5, 20, 0.1)]).cap, null);
  assert.match(suggestCap([step(5, 20, 0.1)]).why, /under 10/);
  assert.equal(suggestCap([step(10, 20, null)]).cap, null);
  assert.match(suggestCap([step(10, 20, null)]).why, /not read/);
  assert.equal(suggestCap([]).cap, null);
});

test("a step with an error, a closed connection or a table that would not start is not good, however quick", () => {
  const faulty = (tables, change) => ({ ...step(tables, 20, 0.1), errors: 0, drops: 0, failedToStart: 0, ...change });
  assert.equal(suggestCap([faulty(10, {}), faulty(20, {})]).cap, 20);
  assert.equal(suggestCap([faulty(10, {}), faulty(20, { errors: 1 }), faulty(40, {})]).cap, 10);
  assert.equal(suggestCap([faulty(10, {}), faulty(20, { drops: 2 }), faulty(40, {})]).cap, 10);
  assert.equal(suggestCap([faulty(10, {}), faulty(20, { failedToStart: 1 }), faulty(40, {})]).cap, 10);
  const none = suggestCap([faulty(10, { errors: 3 })]);
  assert.equal(none.cap, null);
  assert.match(none.why, /errors, drops or tables that failed to start/);
});

test("the limits of the rule can be moved", () => {
  assert.equal(suggestCap([step(10, 100, 0.4), step(20, 150, 0.45)], { p99Ms: 120, cpuShare: 0.5 }).cap, 10);
  assert.equal(suggestCap([step(10, 100, 0.4), step(20, 150, 0.45)], { p99Ms: 200, cpuShare: 0.4 }).cap, null);
});

test("the summary has a row for each step, and dashes where nothing was read", () => {
  const rows = summaryTable("solo", [
    { tables: 10, connections: 10, gameThreads: 10, latency: { count: 500, p50: 2.2, p95: 9.6, p99: 20.4, max: 80 },
      cpuShare: 0.25, cpuCores: 0.5, memoryMb: 61, gamesFinished: 0, errors: 0, drops: 0 },
    { tables: 20, latency: { count: 0, p50: null, p95: null, p99: null, max: null },
      cpuShare: null, cpuCores: null, memoryMb: null, gamesFinished: 1, errors: 2, drops: 1 },
  ], { cores: 2, hold: 180 }).split("\n");
  assert.equal(rows.length, 4);
  assert.match(rows[0], /180 s held at each step, on 2 cores/);
  assert.match(rows[2], /^\s*10\s+10\s+10\s+500\s+2\s+10\s+20\s+80\s+25\s+0\.50\s+61\s+0\s+0\s+0$/);
  assert.match(rows[3], /^\s*20\s+-\s+-\s+0\s+-\s+-\s+-\s+-\s+-\s+-\s+-\s+1\s+2\s+1$/);
});

test("the memory column says whose memory it is, and a note under the table says what that is", () => {
  const rows = (kind) => summaryTable("solo", [
    { tables: 10, latency: { count: 5, p50: 1, p95: 1, p99: 1, max: 1 }, cpuShare: 0.1, cpuCores: 0.2, memoryMb: 61, memoryKind: kind, gamesFinished: 0, errors: 0, drops: 0 },
  ], { cores: 2, hold: 180 }).split("\n");
  assert.match(rows("peak")[1], /cores {2}peak MB {2}games/);
  assert.match(rows("peak")[3], /^memory: peak resident memory since the server started, from its status line$/);
  assert.match(rows("current")[1], /cores {2}cur MB {2}games/);
  assert.match(rows("current")[3], /^memory: resident memory when read, from \/proc or docker stats$/);
  // Nothing read, so nothing to say about it.
  const unread = summaryTable("solo", [{ tables: 10, latency: { count: 0, p50: null, p95: null, p99: null, max: null }, cpuShare: null, cpuCores: null, memoryMb: null, memoryKind: null, gamesFinished: 0, errors: 0, drops: 0 }], { cores: 2, hold: 180 });
  assert.match(unread, /cores {2}mem MB {2}games/);
  assert.equal(unread.split("\n").length, 3);
});

test("a person bids the first bid that is allowed, and plays the first card that is legal", () => {
  assert.deepEqual(chooseAnswer({ id: 3, kind: "bet", max: 5, forbidden: null }), { type: "bet", prompt: 3, value: 0 });
  assert.deepEqual(chooseAnswer({ id: 4, kind: "bet", max: 5, forbidden: 0 }), { type: "bet", prompt: 4, value: 1 });
  assert.deepEqual(chooseAnswer({ id: 5, kind: "play", legal: [2, 4], hand: ["AS", "TD", "4C"] }), { type: "play", prompt: 5, index: 2 });
  assert.deepEqual(chooseAnswer({ id: 6, kind: "play", legal: [0], blind: true, handSize: 1 }), { type: "play", prompt: 6, index: 0 });
});

test("a solo table is one person and five Reckoner Gammas, and an online one six open seats", () => {
  const solo = soloGame(4);
  assert.equal(solo.type, "createSolo");
  assert.equal(solo.settings.botSpeed, 4);
  assert.equal(solo.settings.seats.length, 6);
  assert.deepEqual(solo.settings.seats.map((seat) => seat.kind), ["human", "computer", "computer", "computer", "computer", "computer"]);
  assert.ok(solo.settings.seats.slice(1).every((seat) => seat.level === "reckoner-gamma"));
  assert.equal(new Set(solo.settings.seats.map((seat) => seat.name)).size, 6);

  const online = onlineTable();
  assert.equal(online.type, "createTable");
  assert.equal(online.settings.seats.length, 6);
  assert.ok(online.settings.seats.every((seat) => seat.kind === "open"));
  assert.ok(online.profile.name);
});
