// Pac-Man World Rally web port: static file server + online room relay.
//
//   node server.js [--game <dir>] [--port 8080] [--web <dist dir>]
//
// Rooms are virtual LANs. Each member gets an address 10.77.0.N; binary packets
// from the page (16-byte header, see runtime/net.c) are routed by destination
// address, broadcasts go to every other member. JSON text messages manage rooms.
'use strict';
const http = require('http');
const fs = require('fs');
const path = require('path');
const crypto = require('crypto');

const args = process.argv.slice(2);
const opt = (name, def) => { const i = args.indexOf('--' + name); return i >= 0 ? args[i + 1] : def; };
const HERE = __dirname;
const WEB = path.resolve(opt('web', path.join(HERE, '..', 'dist')));
// game files: --game, else a "game" folder in the deploy package, else the original install next to the project
const GAME = path.resolve(opt('game', [path.join(HERE, '..', 'game'), path.join(HERE, '..', '..', 'PMWR')]
  .find(p => fs.existsSync(path.join(p, 'PMR.exe'))) || path.join(HERE, '..', 'game')));
const PORT = parseInt(opt('port', process.env.PORT || '8080'), 10);
const MAX_PLAYERS = 4; // the game's network mode supports up to 4 human players

const MIME = {
  '.html': 'text/html; charset=utf-8', '.js': 'text/javascript', '.wasm': 'application/wasm', '.json': 'application/json',
  '.ogg': 'audio/ogg', '.css': 'text/css', '.png': 'image/png', '.ini': 'text/plain',
};

// ------------------------------------------------------------------ static files
function serveFile(req, res, root, rel) {
  const full = path.resolve(root, rel);
  if (!full.startsWith(root)) { res.writeHead(403); res.end(); return; }
  fs.stat(full, (err, st) => {
    if (err || !st.isFile()) { res.writeHead(404); res.end('not found'); return; }
    const headers = {'Content-Type': MIME[path.extname(full).toLowerCase()] || 'application/octet-stream',
                     'Content-Length': st.size, 'Cache-Control': 'no-cache', 'Accept-Ranges': 'bytes'};
    /* single byte ranges (the lobby reads track pictures out of Intrface.xpc) */
    const m = /^bytes=(\d+)-(\d*)$/.exec(req.headers.range || '');
    if (m) {
      const start = Number(m[1]), end = Math.min(m[2] ? Number(m[2]) : st.size - 1, st.size - 1);
      if (start > end) { res.writeHead(416, {'Content-Range': 'bytes */' + st.size}); res.end(); return; }
      res.writeHead(206, {...headers, 'Content-Length': end - start + 1, 'Content-Range': `bytes ${start}-${end}/${st.size}`});
      if (req.method === 'HEAD') { res.end(); return; }
      fs.createReadStream(full, {start, end}).pipe(res);
      return;
    }
    res.writeHead(200, headers);
    if (req.method === 'HEAD') { res.end(); return; }
    fs.createReadStream(full).pipe(res);
  });
}

// Game files are only served by a local install (development). A public server runs without
// them (--no-game, or no game folder): each player then loads a .zip of their own copy, which
// web/gamestore.js keeps in the browser.
const HAS_GAME = !args.includes('--no-game') && fs.existsSync(path.join(GAME, 'PMR.exe'));

function listFiles(root, base) {
  const out = [];
  const walk = (dir, rel) => {
    for (const ent of fs.readdirSync(dir, {withFileTypes: true})) {
      if (ent.name.startsWith('.') || ent.name.toLowerCase() === 'dont need a crack') continue;
      const r = rel ? rel + '/' + ent.name : ent.name;
      if (ent.isDirectory()) walk(path.join(dir, ent.name), r);
      else out.push(base ? [r, fs.statSync(path.join(dir, ent.name)).size, base] : [r, fs.statSync(path.join(dir, ent.name)).size]);
    }
  };
  try { walk(root, ''); } catch (e) {}
  return out;
}

function serveManifest(res) {
  const game = HAS_GAME ? listFiles(GAME, null) : [];
  const names = new Set(game.map(g => g[0].toLowerCase()));
  const overlay = listFiles(path.join(WEB, 'overlay'), 'overlay/').filter(o => !names.has(o[0].toLowerCase()));
  const body = JSON.stringify({gameOnServer: HAS_GAME, files: game.concat(overlay)});
  res.writeHead(200, {'Content-Type': 'application/json', 'Cache-Control': 'no-cache'});
  res.end(body);
}

// optional site password (HTTP Basic auth): config.json {"user": "...", "password": "..."} next to the
// server folder, or the PMWR_USER / PMWR_PASSWORD environment variables. The room relay (/ws) carries
// no game files, so it stays open: browsers do not reliably send Basic credentials on WebSocket upgrades.
const CONFIG = (() => { try { return JSON.parse(fs.readFileSync(path.join(HERE, '..', 'config.json'), 'utf8')); } catch (e) { return {}; } })();
const AUTH_USER = process.env.PMWR_USER || CONFIG.user || 'amigos';
const AUTH_PASS = process.env.PMWR_PASSWORD || CONFIG.password || '';
function authorized(req) {
  if (!AUTH_PASS) return true;
  const h = req.headers.authorization || '';
  if (!h.startsWith('Basic ')) return false;
  const [u, ...rest] = Buffer.from(h.slice(6), 'base64').toString('utf8').split(':');
  const a = Buffer.from(u + '\0' + rest.join(':')), b = Buffer.from(AUTH_USER + '\0' + AUTH_PASS);
  return a.length === b.length && crypto.timingSafeEqual(a, b);
}

const server = http.createServer((req, res) => {
  if (!authorized(req)) {
    res.writeHead(401, {'WWW-Authenticate': 'Basic realm="Pac-Man World Rally", charset="UTF-8"', 'Content-Type': 'text/plain; charset=utf-8'});
    res.end('Password required.');
    return;
  }
  let p = decodeURIComponent(req.url.split('?')[0]);
  if (p === '/') p = '/index.html';
  if (p === '/manifest.json') { serveManifest(res); return; }
  if (p.startsWith('/game/')) { if (HAS_GAME) serveFile(req, res, GAME, p.slice(6)); else { res.writeHead(404); res.end(); } }
  else serveFile(req, res, WEB, p.slice(1));
});

// ------------------------------------------------------------------ minimal WebSocket (RFC 6455)
class WS {
  constructor(socket) {
    this.socket = socket;
    this.buf = Buffer.alloc(0);
    this.frags = [];
    this.open = true;
    this.onmessage = () => {};
    this.onclose = () => {};
    socket.setNoDelay(true);
    socket.on('data', d => this.feed(d));
    socket.on('close', () => this.closed());
    socket.on('error', () => this.closed());
  }
  closed() { if (this.open) { this.open = false; this.onclose(); } }
  feed(d) {
    this.buf = this.buf.length ? Buffer.concat([this.buf, d]) : d;
    for (;;) {
      if (this.buf.length < 2) return;
      const b0 = this.buf[0], b1 = this.buf[1];
      let len = b1 & 0x7f, off = 2;
      if (len === 126) { if (this.buf.length < 4) return; len = this.buf.readUInt16BE(2); off = 4; }
      else if (len === 127) { if (this.buf.length < 10) return; len = Number(this.buf.readBigUInt64BE(2)); off = 10; }
      const masked = b1 & 0x80;
      const need = off + (masked ? 4 : 0) + len;
      if (len > 16 * 1024 * 1024) { this.socket.destroy(); return; }
      if (this.buf.length < need) return;
      let payload = this.buf.subarray(off + (masked ? 4 : 0), need);
      if (masked) {
        const mask = this.buf.subarray(off, off + 4);
        payload = Buffer.from(payload);
        for (let i = 0; i < payload.length; i++) payload[i] ^= mask[i & 3];
      }
      this.buf = this.buf.subarray(need);
      const op = b0 & 0x0f, fin = b0 & 0x80;
      if (op === 8) { this.sendFrame(8, Buffer.alloc(0)); this.socket.end(); this.closed(); return; }
      if (op === 9) { this.sendFrame(10, payload); continue; }
      if (op === 10) continue;
      if (op !== 0) this.fragOp = op;
      this.frags.push(payload);
      if (!fin) continue;
      const msg = this.frags.length === 1 ? this.frags[0] : Buffer.concat(this.frags);
      this.frags = [];
      this.onmessage(this.fragOp === 1 ? msg.toString('utf8') : msg);
    }
  }
  sendFrame(op, data) {
    if (!this.open) return;
    const len = data.length;
    let hdr;
    if (len < 126) hdr = Buffer.from([0x80 | op, len]);
    else if (len < 65536) { hdr = Buffer.alloc(4); hdr[0] = 0x80 | op; hdr[1] = 126; hdr.writeUInt16BE(len, 2); }
    else { hdr = Buffer.alloc(10); hdr[0] = 0x80 | op; hdr[1] = 127; hdr.writeBigUInt64BE(BigInt(len), 2); }
    this.socket.write(Buffer.concat([hdr, data]));
  }
  send(data) {
    if (typeof data === 'string') this.sendFrame(1, Buffer.from(data, 'utf8'));
    else this.sendFrame(2, data);
  }
}

server.on('upgrade', (req, socket) => {
  if (!req.url.startsWith('/ws') || req.headers.upgrade?.toLowerCase() !== 'websocket') { socket.destroy(); return; }
  const key = req.headers['sec-websocket-key'];
  const accept = crypto.createHash('sha1').update(key + '258EAFA5-E914-47DA-95CA-C5AB0DC85B11').digest('base64');
  socket.write('HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n' +
               'Sec-WebSocket-Accept: ' + accept + '\r\n\r\n');
  onClient(new WS(socket));
});

// ------------------------------------------------------------------ rooms
const rooms = new Map();          // code -> room
const CODE_CHARS = 'ABCDEFGHJKLMNPQRSTUVWXYZ23456789';

function newCode() {
  for (;;) {
    let c = '';
    for (let i = 0; i < 6; i++) c += CODE_CHARS[crypto.randomInt(CODE_CHARS.length)];
    if (!rooms.has(c)) return c;
  }
}

const ipBytes = n => [10, 77, 0, n];
const ipKey = b => b.join('.');

function roomInfo(r) {
  return {code: r.code, name: r.name, public: r.public, players: r.members.size, max: MAX_PLAYERS,
          owner: r.owner ? r.owner.name : '', playing: r.playing, settings: r.settings, startSeq: r.startSeq,
          hostReady: r.hostReady || 0};
}

// match settings chosen by the owner (validated loosely; the page knows the game's option lists)
function cleanSettings(s) {
  const n = (v, lo, hi, d) => Number.isInteger(v) && v >= lo && v <= hi ? v : d;
  s = s || {};
  return {mode: n(s.mode, 0, 7, 0), world: n(s.world, 0, 63, 0), option: n(s.option, 0, 10000000, 0),
          cpu: n(s.cpu, 0, 4, 0), maxPlayers: n(s.maxPlayers, 2, 4, 4)};
}

function memberList(r) {
  return [...r.members.values()].map(m => ({name: m.name, ip: ipKey(m.ip), owner: m === r.owner, away: !!m.away, char: m.char || 0}));
}

const sendTo = (m, data) => { if (m.ws && !m.away && m.ws.open) m.ws.send(data); };

function broadcastMembers(r) {
  const msg = JSON.stringify({t: 'members', members: memberList(r), room: roomInfo(r)});
  for (const m of r.members.values()) sendTo(m, msg);
}

function leave(c) {
  const r = c.room;
  if (!r) return;
  log('leave', c.name, r.code);
  clearTimeout(c.awayTimer);
  r.members.delete(ipKey(c.ip));
  c.room = null;
  const left = JSON.stringify({t: 'left', ip: ipKey(c.ip), name: c.name});
  for (const m of r.members.values()) sendTo(m, left);
  if (!r.members.size) { rooms.delete(r.code); return; }
  if (r.owner === c) { r.owner = [...r.members.values()].find(m => !m.away) || r.members.values().next().value; r.hostReady = 0; }
  broadcastMembers(r);
}

// a page reload (between matches) keeps the seat: same address, same owner, for 30 s
function goAway(c) {
  if (!c.room) return;
  c.away = true;
  c.awayTimer = setTimeout(() => leave(c), 30000);
  broadcastMembers(c.room);
}

const log = (...a) => { if (process.env.PMWR_DEBUG) console.log(new Date().toISOString().slice(11, 23), ...a); };

function join(c, r, token) {
  log('join', c.name, r.code, token ? 'token ' + token.slice(0, 6) : 'new');
  const seat = token ? [...r.members.values()].find(m => m.token === token && m !== c) : null;
  if (!seat && r.members.size >= MAX_PLAYERS) { c.ws.send(JSON.stringify({t: 'error', msg: 'The room is full.'})); return; }
  leave(c);
  if (seat) {
    clearTimeout(seat.awayTimer);
    log('reclaim seat of', seat.name, 'old socket open:', !!(seat.ws && seat.ws.open), 'same ws:', seat.ws === c.ws);
    if (seat.ws && seat.ws.open && seat.ws !== c.ws) seat.ws.socket.destroy();
    seat.room = null;
    c.ip = seat.ip;
    c.token = seat.token;
    r.members.set(ipKey(c.ip), c);
    if (r.owner === seat) r.owner = c;
  } else {
    let n = 2;
    while ([...r.members.values()].some(m => m.ip[3] === n)) n++;
    c.ip = ipBytes(n);
    c.token = crypto.randomBytes(12).toString('hex');
    r.members.set(ipKey(c.ip), c);
  }
  c.room = r;
  c.away = false;
  if ([...r.members.values()].some(o => o !== c && o.char === c.char)) {
    for (let k = 0; k < 14; k++) if (![...r.members.values()].some(o => o !== c && o.char === k)) { c.char = k; break; }
  }
  if (!r.owner) r.owner = c;
  c.ws.send(JSON.stringify({t: 'joined', ip: ipKey(c.ip), token: c.token, room: roomInfo(r), members: memberList(r)}));
  broadcastMembers(r);
}

const cleanName = (s, def) => (String(s || '').replace(/[^\p{L}\p{N} _.\-!?]/gu, '').trim().slice(0, 20) || def);

function onClient(ws) {
  const c = {ws, name: 'Player', room: null, ip: null, char: 0};
  ws.onmessage = msg => {
    if (typeof msg !== 'string') { route(c, msg); return; }
    let m;
    try { m = JSON.parse(msg); } catch (e) { return; }
    switch (m.t) {
      case 'hello':
        c.name = cleanName(m.name, 'Player');
        if (Number.isInteger(m.char) && m.char >= 0 && m.char < 16) c.char = m.char;
        break;
      case 'list':
        ws.send(JSON.stringify({t: 'rooms', rooms: [...rooms.values()].filter(r => r.public).map(roomInfo)}));
        break;
      case 'create': {
        const r = {code: newCode(), name: cleanName(m.name, c.name + "'s room"), public: !!m.public,
                   members: new Map(), owner: null, playing: false, created: Date.now(),
                   settings: cleanSettings(m.settings), startSeq: 0};
        rooms.set(r.code, r);
        join(c, r);
        break;
      }
      case 'join': {
        const r = rooms.get(String(m.code || '').toUpperCase().trim());
        if (!r) ws.send(JSON.stringify({t: 'error', msg: 'Room not found. Check the code.'}));
        else join(c, r, typeof m.token === 'string' ? m.token : null);
        break;
      }
      case 'leave': leave(c); break;
      case 'ping': ws.send(JSON.stringify({t: 'pong', id: m.id})); break;
      case 'char': /* character picked in the web lobby (index in the game's roster); one per player */
        if (Number.isInteger(m.char) && m.char >= 0 && m.char < 16) {
          if (c.room && [...c.room.members.values()].some(o => o !== c && o.char === m.char))
            ws.send(JSON.stringify({t: 'error', msg: 'Another player already picked that character.'}));
          else c.char = m.char;
        }
        if (c.room) broadcastMembers(c.room);
        break;
      case 'needLobby': /* a player's game cannot find the owner's match: the owner opens a new one */
        if (c.room && c.room.owner && c.room.owner !== c) sendTo(c.room.owner, JSON.stringify({t: 'needLobby', name: c.name}));
        break;
      case 'go': /* the owner's game started the match */
        if (c.room && c.room.owner === c) {
          const out = JSON.stringify({t: 'go'});
          for (const mm of c.room.members.values()) if (mm !== c) sendTo(mm, out);
        }
        break;
      case 'matchReady': /* this player's game is in the match lobby with its character confirmed */
        if (c.room) {
          const out = JSON.stringify({t: 'matchReady', ip: ipKey(c.ip), ver: Number(m.ver) || 0});
          for (const mm of c.room.members.values()) sendTo(mm, out);
        }
        break;
      case 'matchFailed': /* a player's game could not get into the match: tell everybody */
        if (c.room) {
          const out = JSON.stringify({t: 'matchFailed', ip: ipKey(c.ip), name: c.name, msg: String(m.msg || '').slice(0, 200)});
          for (const mm of c.room.members.values()) sendTo(mm, out);
        }
        break;
      case 'playing':
        if (c.room && c.room.owner === c) { c.room.playing = !!m.value; broadcastMembers(c.room); }
        break;
      case 'settings':
        if (c.room && c.room.owner === c) { c.room.settings = cleanSettings(m.settings); broadcastMembers(c.room); }
        break;
      case 'start':
        if (c.room && c.room.owner === c) {
          const r = c.room;
          r.playing = true;
          r.startSeq++;
          const out = JSON.stringify({t: 'start', settings: r.settings, host: ipKey(c.ip), seq: r.startSeq});
          for (const mm of r.members.values()) sendTo(mm, out);
          broadcastMembers(r);
        }
        break;
      case 'hostReady': /* the owner's game has a match lobby open; ver identifies that lobby */
        if (c.room && c.room.owner === c) {
          c.room.hostReady = Number(m.ver) || 0;
          if (c.room.playing) { c.room.playing = false; broadcastMembers(c.room); } /* back in a lobby: no match running */
          const out = JSON.stringify({t: 'hostReady', ver: c.room.hostReady});
          for (const mm of c.room.members.values()) if (mm !== c) sendTo(mm, out);
        }
        break;
      case 'kick':
        if (c.room && c.room.owner === c) {
          const target = c.room.members.get(String(m.ip));
          if (target && target !== c) {
            sendTo(target, JSON.stringify({t: 'kicked'}));
            leave(target);
          }
        }
        break;
      case 'rename':
        if (c.room && c.room.owner === c) { c.room.name = cleanName(m.name, c.room.name); broadcastMembers(c.room); }
        break;
      case 'chat':
        if (c.room) {
          const out = JSON.stringify({t: 'chat', name: c.name, text: String(m.text || '').slice(0, 200)});
          for (const mm of c.room.members.values()) sendTo(mm, out);
        }
        break;
    }
  };
  ws.onclose = () => { log('close', c.name, c.room ? c.room.code : '-'); goAway(c); };
}

// binary packet: [type][flags][res x2][src ip x4][dst ip x4][src port x2][dst port x2] payload
function route(c, pkt) {
  const r = c.room;
  if (!r || pkt.length < 16) return;
  pkt[4] = c.ip[0]; pkt[5] = c.ip[1]; pkt[6] = c.ip[2]; pkt[7] = c.ip[3];
  const dst = [pkt[8], pkt[9], pkt[10], pkt[11]];
  if (dst[3] === 255 || dst[0] === 255) {
    for (const m of r.members.values()) if (m !== c) sendTo(m, pkt);
    return;
  }
  const m = r.members.get(ipKey(dst));
  if (m) sendTo(m, pkt);
}

server.listen(PORT, () => {
  console.log(`Pac-Man World Rally web: http://localhost:${PORT}/  (web: ${WEB}, game files: ${HAS_GAME ? GAME : 'none, players upload their own .zip'})`);
});
