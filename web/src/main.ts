import { socketUrl } from "./net/socket";

function byId<T extends HTMLElement>(id: string): T {
  const found = document.getElementById(id);
  if (!found) {
    throw new Error(`The page has no #${id}`);
  }
  return found as T;
}

const status = byId<HTMLOutputElement>("status");
const form = byId<HTMLFormElement>("send");
const input = byId<HTMLInputElement>("text");
const log = byId<HTMLOListElement>("log");

const socket = new WebSocket(socketUrl(window.location));

socket.addEventListener("open", () => {
  status.textContent = "connected";
});

socket.addEventListener("close", (event) => {
  status.textContent = `closed (${event.code})`;
});

socket.addEventListener("message", (event) => {
  const item = document.createElement("li");
  item.textContent = String(event.data);
  log.append(item);
});

form.addEventListener("submit", (event) => {
  event.preventDefault();
  if (socket.readyState === WebSocket.OPEN && input.value !== "") {
    socket.send(input.value);
    input.value = "";
  }
});
