// The server's WebSocket is /ws on whichever host served the page.
export function socketUrl(location: Pick<Location, "protocol" | "host">): string {
  const scheme = location.protocol === "https:" ? "wss:" : "ws:";
  return `${scheme}//${location.host}/ws`;
}
