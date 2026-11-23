// Madden Arcade Unleashed relay: lets two players connect with a code when neither can accept connections from the
// internet (no router setup). Each player's launcher and game connect OUT to this server over a WebSocket:
//   wss://<server>/room/<CODE>/<kind>?role=host|join      kind = "party" (launcher ready-up) or "game" (the match)
// The host and the joiner of the same room and kind are paired and every binary message one sends is passed to the
// other unchanged. Text messages from the server: "peer:up" / "peer:down" when the other side connects or leaves.
// Nothing is stored; a room exists only while someone is connected to it.
const http = require("http");
const { WebSocketServer } = require("ws");

const PORT = process.env.PORT || 8080;
const MAX_MSG = 256 * 1024;
const rooms = new Map();   // "CODE/kind" -> { host, join }

const server = http.createServer((req, res) => {
  if (req.url === "/" || req.url === "/health") {   // also used to wake a sleeping free server before a game
    res.writeHead(200, { "content-type": "text/plain" });
    res.end("Madden Arcade Unleashed relay - ok\n");
    return;
  }
  res.writeHead(404); res.end();
});
const wss = new WebSocketServer({ server, maxPayload: MAX_MSG });

wss.on("connection", (ws, req) => {
  const u = new URL(req.url, "http://x");
  const m = u.pathname.match(/^\/room\/([A-Z0-9]{4,12})\/(party|game)$/);
  const role = u.searchParams.get("role");
  if (!m || (role !== "host" && role !== "join")) { ws.close(1008, "bad room"); return; }
  const key = m[1] + "/" + m[2];
  let room = rooms.get(key);
  if (!room) { if (rooms.size > 5000) { ws.close(1013, "busy"); return; } room = {}; rooms.set(key, room); }
  const other = role === "host" ? "join" : "host";
  if (room[role]) { try { room[role].close(1000, "replaced"); } catch {} }
  room[role] = ws;
  ws.isAlive = true;
  ws.on("pong", () => { ws.isAlive = true; });
  if (room[other]) { ws.send("peer:up"); room[other].send("peer:up"); }
  ws.on("message", (data, isBinary) => {
    if (!isBinary) return;
    const peer = room[other];
    if (peer && peer.readyState === 1) peer.send(data, { binary: true });
  });
  ws.on("close", () => {
    if (room[role] !== ws) return;
    delete room[role];
    if (room[other]) { try { room[other].send("peer:down"); } catch {} }
    if (!room.host && !room.join) rooms.delete(key);
  });
});
// drop connections that stopped answering
setInterval(() => {
  for (const ws of wss.clients) { if (!ws.isAlive) { ws.terminate(); continue; } ws.isAlive = false; try { ws.ping(); } catch {} }
}, 20000);

server.listen(PORT, () => console.log("relay listening on " + PORT));
