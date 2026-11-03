import { describe, expect, it } from "vitest";
import { socketUrl } from "../../src/net/socket";

describe("socketUrl", () => {
  it("uses ws: on a page served over http", () => {
    expect(socketUrl({ protocol: "http:", host: "127.0.0.1:8080" })).toBe("ws://127.0.0.1:8080/ws");
  });

  it("uses wss: on a page served over https", () => {
    expect(socketUrl({ protocol: "https:", host: "whist.example" })).toBe("wss://whist.example/ws");
  });

  it("keeps the dev server's port, which passes /ws through", () => {
    expect(socketUrl({ protocol: "http:", host: "localhost:5173" })).toBe("ws://localhost:5173/ws");
  });
});
