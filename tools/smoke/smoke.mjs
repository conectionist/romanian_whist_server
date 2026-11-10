#!/usr/bin/env node
// Plays the first moments of a solo game against a running server, to see that it works end to end.
// Usage and options: the README, under Tests.

import { pathToFileURL } from "node:url";
import { parseArgs } from "node:util";

const protocol = 1;

const sleep = (ms) => new Promise((resolve) => setTimeout(resolve, ms));

class Connection {
  inbox = [];
  wake = null;
  ended = null;

  constructor(url) {
    this.socket = new WebSocket(url);
    this.socket.addEventListener("open", () => this.notify());
    this.socket.addEventListener("message", (event) => {
      this.inbox.push(event.data);
      this.notify();
    });
    this.socket.addEventListener("error", (event) => {
      this.ended ??= `failed${event.message ? `: ${event.message}` : ""}`;
      this.notify();
    });
    this.socket.addEventListener("close", (event) => {
      this.ended ??= `closed with code ${event.code}`;
      this.notify();
    });
  }

  notify() {
    const wake = this.wake;
    this.wake = null;
    wake?.();
  }

  async happening(deadline) {
    const left = deadline - Date.now();
    if (left <= 0) {
      return;
    }
    await new Promise((resolve) => {
      const timer = setTimeout(resolve, left);
      this.wake = () => {
        clearTimeout(timer);
        resolve();
      };
    });
  }

  async opened(deadline) {
    while (this.socket.readyState === WebSocket.CONNECTING) {
      if (this.ended || Date.now() >= deadline) {
        throw new Error(`/ws did not open: ${this.ended ?? "timed out"}`);
      }
      await this.happening(deadline);
    }
    if (this.socket.readyState !== WebSocket.OPEN) {
      throw new Error(`/ws did not open: ${this.ended ?? "closed"}`);
    }
  }

  send(message) {
    this.socket.send(JSON.stringify(message));
  }

  async receive(deadline, what) {
    while (this.inbox.length === 0) {
      if (this.ended) {
        throw new Error(`the connection ${this.ended} while waiting for ${what}`);
      }
      if (Date.now() >= deadline) {
        throw new Error(`timed out waiting for ${what}`);
      }
      await this.happening(deadline);
    }
    const text = this.inbox.shift();
    let message;
    try {
      message = JSON.parse(text);
    } catch {
      throw new Error(`waiting for ${what}, a message that is not JSON came: ${String(text).slice(0, 80)}`);
    }
    if (message.type === "error") {
      throw new Error(`waiting for ${what}, the server sent an error: ${JSON.stringify(message)}`);
    }
    return message;
  }

  close() {
    this.socket.close();
  }
}

export async function waitForPage(baseUrl, { timeoutMs = 10000 } = {}) {
  const deadline = Date.now() + timeoutMs;
  let last = "no answer";
  for (;;) {
    try {
      const response = await fetch(new URL("/", baseUrl), { signal: AbortSignal.timeout(5000) });
      await response.arrayBuffer();
      const type = response.headers.get("content-type") ?? "";
      if (response.status === 200 && type.startsWith("text/html")) {
        return;
      }
      last = `${response.status} ${type}`;
    } catch (error) {
      last = error.cause?.code ?? error.message;
    }
    if (Date.now() >= deadline) {
      throw new Error(`GET / did not give 200 text/html in time (last: ${last})`);
    }
    await sleep(500);
  }
}

const soloGame = {
  type: "createSolo",
  settings: {
    structure: "1-8-1",
    seats: [
      { kind: "human", name: "Ana", gender: "female", avatar: "woman-long-red-hair" },
      { kind: "computer", level: "cyborg", name: "Bogdan", gender: "male", avatar: "man-dark-hair-beard" },
    ],
  },
};

export async function smokeGame(baseUrl, { commit, timeoutMs = 10000 } = {}) {
  const url = new URL("/ws", baseUrl);
  url.protocol = url.protocol === "https:" ? "wss:" : "ws:";

  const deadline = Date.now() + timeoutMs;
  const connection = new Connection(url);
  try {
    await connection.opened(deadline);
    connection.send({ type: "hello", protocol, lang: "en" });

    const welcome = await connection.receive(deadline, "the welcome");
    if (welcome.type !== "welcome") {
      throw new Error(`expected a welcome first, got ${welcome.type}`);
    }
    const server = welcome.server ?? {};
    if (typeof server.version !== "string" || server.version === "") {
      throw new Error(`the welcome carries no server version: ${JSON.stringify(welcome)}`);
    }
    if (commit !== undefined && server.commit !== commit) {
      throw new Error(`the server reports commit "${server.commit}", expected "${commit}"`);
    }

    const sync = await connection.receive(deadline, "the sync");
    if (sync.type !== "sync") {
      throw new Error(`expected a sync after the welcome, got ${sync.type}`);
    }

    connection.send(soloGame);
    for (;;) {
      const message = await connection.receive(deadline, "a prompt");
      if (message.type === "prompt") {
        return { version: server.version, commit: server.commit, prompt: message.kind };
      }
    }
  } finally {
    connection.close();
  }
}

async function main() {
  const { values } = parseArgs({
    options: {
      url: { type: "string", default: "http://127.0.0.1:8080" },
      commit: { type: "string" },
      timeout: { type: "string", default: "10" },
    },
  });
  const timeoutMs = Number(values.timeout) * 1000;
  if (!(timeoutMs > 0)) {
    throw new Error("--timeout must be a number of seconds above 0");
  }

  await waitForPage(values.url, { timeoutMs });
  console.log(`ok   GET / gives a page at ${values.url}`);

  const seen = await smokeGame(values.url, { commit: values.commit, timeoutMs });
  console.log(`ok   welcome: server ${seen.version}, commit ${seen.commit || "(none)"}`);
  console.log(`ok   a solo game asked for a ${seen.prompt}`);
}

if (process.argv[1] && import.meta.url === pathToFileURL(process.argv[1]).href) {
  main().then(() => process.exit(0), (error) => {
    console.error(`FAIL ${error.message}`);
    process.exit(1);
  });
}
