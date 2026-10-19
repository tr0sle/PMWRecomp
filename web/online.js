// Online rooms for the web port: lobby + room UI, WebSocket link to server/server.js,
// and a driver that takes everybody's game straight into the room's match.
// Everything about the match (mode, track, CPUs, characters) is chosen in the web room; when the
// owner starts, each game boots behind a "starting match" screen, the driver sets the match up
// through the game's own NETWORK PLAY menus out of sight, and the screen lifts when the race loads.
// While in a room, Module.net is the virtual network adapter used by runtime/net.c.
'use strict';
(function () {
  const el = (tag, attrs = {}, ...kids) => {
    const e = document.createElement(tag);
    for (const [k, v] of Object.entries(attrs)) {
      if (k === 'class') e.className = v;
      else if (k.startsWith('on')) e.addEventListener(k.slice(2), v);
      else if (k === 'value') e.value = v;
      else if (v !== false && v != null) e.setAttribute(k, v === true ? '' : v);
    }
    for (const k of kids.flat()) if (k != null) e.append(k.nodeType ? k : document.createTextNode(String(k)));
    return e;
  };
  const sleep = ms => new Promise(r => setTimeout(r, ms));
  const ipToInt = s => { const p = s.split('.').map(Number); return (p[0] | p[1] << 8 | p[2] << 16 | p[3] << 24) >>> 0; };
  const store = {
    get(k, d) { try { return localStorage.getItem(k) ?? d; } catch (e) { return d; } },
    set(k, v) { try { localStorage.setItem(k, v); } catch (e) {} },
  };
  const params = new URLSearchParams(location.search);
  /* per-tab memory of the room seat (frames of one tab share sessionStorage, so the key includes the frame's player name) */
  const skey = k => k + ':' + (params.get('name') || '');
  const session = {
    get(k) { try { return JSON.parse(sessionStorage.getItem(skey(k))); } catch (e) { return null; } },
    set(k, v) { try { sessionStorage.setItem(skey(k), JSON.stringify(v)); } catch (e) {} },
    del(k) { try { sessionStorage.removeItem(skey(k)); } catch (e) {} },
  };

  const css = `
  #overlay .online { width: min(900px, 100%); }
  .online .modes { display: flex; flex-wrap: wrap; gap: 6px; }
  .online .modes button, .online .pill { font-size: 13px !important; padding: 6px 10px !important; border-radius: 16px !important; background: #232838 !important; color: var(--fg) !important; font-weight: 500 !important; }
  .online .modes button.sel { background: var(--accent) !important; color: #000 !important; font-weight: 700 !important; }
  .online .modes button:disabled:not(.sel) { opacity: .45; }
  .online .tracks { display: grid; grid-template-columns: repeat(auto-fill, minmax(132px, 1fr)); gap: 8px; margin-top: 8px; }
  .online .track { position: relative; border-radius: 8px !important; overflow: hidden; background: #1a1f2c !important; border: 2px solid transparent !important; cursor: pointer; padding: 0 !important; text-align: left; color: var(--fg) !important; font: inherit; font-size: 13px !important; font-weight: 400 !important; }
  .online .track:disabled { cursor: default; }
  .online .track.sel { border-color: var(--accent) !important; box-shadow: 0 0 0 2px #ffd40055; }
  .online .track .pic { display: block; width: 100%; aspect-ratio: 2 / 1; background: #2a3042 center / cover no-repeat; }
  .online .track .pic.mosaic { display: grid; grid-template-columns: 1fr 1fr; grid-template-rows: 1fr 1fr; }
  .online .track .pic.mosaic i { background: center / cover no-repeat; }
  .online .track span { display: block; font-size: 12px; padding: 4px 6px 5px; white-space: nowrap; overflow: hidden; text-overflow: ellipsis; }
  .online .track.sel span { color: var(--accent); font-weight: 700; }
  .online .chars { display: grid; grid-template-columns: repeat(auto-fill, minmax(62px, 1fr)); gap: 6px; }
  .online .char { padding: 0 !important; border: 2px solid transparent !important; border-radius: 10px !important; background: #1a1f2c !important; cursor: pointer; color: var(--fg) !important; font: inherit; font-weight: 400 !important; }
  .online .char.sel { border-color: var(--accent) !important; background: #2d2a12 !important; }
  .online .char:disabled { opacity: .35; cursor: default; }
  .online .char .face, .online .members .face { display: block; width: 100%; aspect-ratio: 1; background: #2a3042 center / contain no-repeat; border-radius: 8px; }
  .online .char small { display: block; font-size: 10px; padding: 2px 0 3px; white-space: nowrap; overflow: hidden; text-overflow: ellipsis; text-align: center; }
  .online .members { list-style: none; padding: 0; margin: 0; display: flex; flex-direction: column; gap: 6px; }
  .online .members li { display: flex; align-items: center; gap: 8px; background: #181c26; border-radius: 8px; padding: 4px 8px 4px 4px; margin: 0; }
  .online .members .face { width: 38px; flex: none; }
  .online .members small { color: var(--muted); display: block; font-size: 11px; }
  .online .opts { display: grid; grid-template-columns: repeat(auto-fit, minmax(190px, 1fr)); gap: 8px 14px; margin-top: 10px; }
  #matchcover { position: fixed; inset: 0; z-index: 17; display: none; align-items: center; justify-content: center; padding: 16px;
    background: radial-gradient(ellipse at center, #1d2a6b 0%, #0a0d1c 70%); }
  #matchcover .card { width: min(520px, 100%); text-align: center; }
  #matchcover .ctitle { font-weight: 800; font-size: 24px; color: var(--accent); letter-spacing: .5px; text-shadow: 0 2px 0 #b35b00; }
  #matchcover .ctrack { width: 100%; aspect-ratio: 2 / 1; margin: 12px 0 6px; border-radius: 12px; background: #2a3042 center / cover no-repeat; box-shadow: 0 8px 30px rgba(0,0,0,.5); }
  #matchcover .cdesc { color: #d7dcf0; font-size: 14px; }
  #matchcover .cplayers { list-style: none; padding: 0; margin: 12px 0 0; display: flex; justify-content: center; gap: 10px; flex-wrap: wrap; }
  #matchcover .cplayers li { width: 84px; font-size: 12px; }
  #matchcover .cplayers .face { display: block; width: 64px; height: 64px; margin: 0 auto 3px; border-radius: 10px; background: #2a3042 center / contain no-repeat; border: 2px solid #3a4152; }
  #matchcover .cplayers li.ready .face { border-color: #58d46a; }
  #matchcover .cplayers small { display: block; color: var(--muted); }
  #matchcover .cplayers li.ready small { color: #58d46a; }
  #matchcover .lbar { position: relative; height: 8px; margin: 16px auto 0; width: 70%; border-radius: 4px; background: #1d2230; overflow: hidden; }
  #matchcover .lfill { position: absolute; top: 0; bottom: 0; width: 35%; border-radius: 4px; background: linear-gradient(90deg, #ffb000, #ffd400); animation: lslide 1.1s ease-in-out infinite; }
  #matchcover .cstep { margin-top: 8px; color: var(--muted); font-size: 13px; min-height: 1.4em; }
  #matchcover .cerr { color: #ffb3b3; margin-top: 10px; font-size: 14px; }
  #matchcover button { margin: 12px 6px 0; font: inherit; font-size: 14px; padding: 7px 16px; border-radius: 8px; border: 0; background: #2a2f3d; color: var(--fg); cursor: pointer; }
  #ping { position: fixed; left: 10px; top: 8px; z-index: 15; font: 600 12px ui-monospace, monospace; padding: 4px 8px; border-radius: 6px; background: rgba(0,0,0,.6); color: #9fe39f; pointer-events: none; display: none; }
  #ping.mid { color: #ffd166; } #ping.bad { color: #ff7b7b; }
  .netbadge { left: auto !important; right: 110px; }`;
  document.head.append(el('style', {}, css));

  // ---------------------------------------------------------------- what the game offers online
  const fmtTime = ms => { const s = ms / 1000; return Math.floor(s / 60) + ':' + String(s % 60).padStart(2, '0'); };
  const TIME_VALUES = Array.from({length: 12}, (_, i) => (i + 1) * 30000);
  /* race tracks in the order of the game's network menu; the index is also their picture's index */
  const TRACKS = ['Cloud Garden', 'Molten Mountain', 'Rolling Hills', 'Ghost Mansion', 'Canyon Crusade', 'Arctic Iceberg',
                  'Jungle Safari', 'Pirate Cove', 'Winding Wetlands', "Toc-Man's Factory", 'Funhouse of Terror', "Spooky's Castle",
                  'Galactic Outpost', "King's Kourse", 'Retro Maze', 'Subterranean Speedway'];
  const ARENAS = [[1, 'Molten Mountain'], [3, 'Ghost Mansion'], [4, 'Canyon Crusade'], [5, 'Arctic Iceberg'],
                  [11, "Spooky's Castle"], [12, 'Galactic Outpost']]; /* [picture, name] */
  const CUPS = [[1, 'Cherry Cup', [0, 1, 2, 3]], [2, 'Grape Cup', [4, 5, 6, 7]], [3, 'Watermelon Cup', [8, 9, 10, 11]],
                [4, 'Classic Cup', [12, 13, 14]]];
  /* the game's roster, in the order of its character select */
  const CHARACTERS = ['Pac-Man', 'Blinky', 'Pooka', 'Toc-Man', 'Inky', 'Clyde', 'Ms. Pac-Man', 'Pinky', 'Pac-Man Jr.',
                      'Spooky', 'Pac-Devil', 'Erwin', 'Fygar', 'Mr. Driller'];
  const MODES = [
    {id: 0, name: 'Race', short: 'Race', kind: 'race'},
    {id: 4, name: 'Circuit (cup)', short: 'Circuit', kind: 'circuit'},
    {id: 1, name: 'Battle: Deathmatch', short: 'Deathmatch', kind: 'battle', opt: {label: 'Kills to win', addr: 0x80ac48, size: 1, values: [5, 10, 15, 20, 25], def: 5, fmt: v => String(v)}},
    {id: 2, name: 'Battle: Free For All', short: 'Free For All', kind: 'battle', opt: {label: 'Time limit', addr: 0x80ac4c, size: 4, values: TIME_VALUES, def: 120000, fmt: fmtTime}},
    {id: 3, name: 'Battle: Last Kart Driving', short: 'Last Kart Driving', kind: 'battle', opt: {label: 'Hit points', addr: 0x80ac50, size: 4, values: [100, 200, 300], def: 100, fmt: v => v + '%'}},
    {id: 6, name: 'Battle: Binge', short: 'Binge', kind: 'battle', opt: {label: 'Time limit', addr: 0x80ac4c, size: 4, values: TIME_VALUES, def: 120000, fmt: fmtTime}},
    {id: 7, name: 'Battle: Classic', short: 'Classic', kind: 'battle', opt: {label: 'Rounds', addr: 0x80ac54, size: 1, values: [1, 2, 3], def: 1, fmt: v => String(v)}},
  ];
  const CPU_LEVELS = ['No CPUs', 'Easy CPUs', 'Normal CPUs', 'Hard CPUs', 'Nightmare CPUs'];
  const modeOf = id => MODES.find(m => m.id === id) || MODES[0];
  /* [value, name, pictures] of the places a mode can be played in */
  const worldsFor = mode => mode.kind === 'race' ? TRACKS.map((n, i) => [i, n, [i]])
    : mode.kind === 'battle' ? ARENAS.map(([pic, n], i) => [i, n, [pic]]) : CUPS.map(([v, n, pics]) => [v, n, pics]);
  const defaultSettings = () => ({mode: 0, world: 0, option: 0, cpu: 2, maxPlayers: 4});
  function describe(s) {
    const m = modeOf(s.mode);
    const w = worldsFor(m).find(([id]) => id === s.world);
    const parts = [m.name, w ? w[1] : '?'];
    if (m.opt) parts.push(m.opt.label + ': ' + m.opt.fmt(s.option || m.opt.def));
    parts.push(CPU_LEVELS[s.cpu] || '', 'up to ' + s.maxPlayers + ' players');
    return parts.filter(Boolean).join(' · ');
  }
  const picsOf = s => { const m = modeOf(s.mode); const w = worldsFor(m).find(([id]) => id === s.world); return w ? w[2] : []; };

  // ---------------------------------------------------------------- pictures from the player's own Intrface.xpc
  // Track pictures (512x256) and character portraits (128x128) are stored there as plain RGBA; only
  // their offsets are known here. They are cut out in the browser and cached as small images.
  const XPC = {name: 'Intrface.xpc', size: 55225200, worlds: 20652648, worldBytes: 512 * 256 * 4, chars: 29819496, charBytes: 128 * 128 * 4};
  const Thumbs = (() => {
    const mem = new Map(), waiting = new Map();
    let usable = null;
    async function available() {
      if (usable !== null) return usable;
      const ent = (Module.manifest || []).find(e => e[0].toLowerCase() === 'intrface.xpc');
      usable = !!ent && ent[1] === XPC.size;
      return usable;
    }
    async function range(start, len) {
      const ent = (Module.manifest || []).find(e => e[0].toLowerCase() === 'intrface.xpc');
      if (ent && ent[2] === 'idb:') return window.GameStore.readRange(ent[0], start, start + len);
      const r = await fetch((Module.gameBase || 'game/') + encodeURI(ent[0]), {headers: {Range: `bytes=${start}-${start + len - 1}`}});
      if (r.status !== 206) { usable = false; return null; } /* never download the whole file for a picture */
      return new Uint8Array(await r.arrayBuffer());
    }
    async function make(key) {
      const cached = store.get('pmwr-pic:' + key, '');
      if (cached) return cached;
      if (!await available()) return '';
      const isChar = key[0] === 'c', k = Number(key.slice(1));
      const w = isChar ? 128 : 512, h = isChar ? 128 : 256;
      const px = await range(isChar ? XPC.chars + k * XPC.charBytes : XPC.worlds + k * XPC.worldBytes, w * h * 4);
      if (!px || px.length !== w * h * 4) return '';
      const src = document.createElement('canvas');
      src.width = w; src.height = h;
      src.getContext('2d').putImageData(new ImageData(new Uint8ClampedArray(px.buffer, px.byteOffset, px.length), w, h), 0, 0);
      const out = document.createElement('canvas');
      out.width = isChar ? 96 : 256; out.height = isChar ? 96 : 128;
      const g = out.getContext('2d');
      g.imageSmoothingQuality = 'high';
      g.drawImage(src, 0, 0, out.width, out.height);
      const url = isChar ? out.toDataURL('image/png') : out.toDataURL('image/jpeg', 0.82);
      store.set('pmwr-pic:' + key, url);
      return url;
    }
    function get(key) {
      if (mem.has(key)) return Promise.resolve(mem.get(key));
      if (!waiting.has(key)) waiting.set(key, make(key).catch(() => '').then(u => { mem.set(key, u); return u; }));
      return waiting.get(key);
    }
    /* sets an element's background to a picture once it is ready */
    function paint(node, key) {
      if (mem.has(key)) { if (mem.get(key)) node.style.backgroundImage = `url(${mem.get(key)})`; return; }
      get(key).then(u => { if (u) node.style.backgroundImage = `url(${u})`; });
    }
    return {get, paint};
  })();
  function trackPic(pics, cls) {
    if (pics.length > 1) {
      const d = el('i', {class: cls + ' mosaic'}, pics.slice(0, 4).map(() => el('i')));
      [...d.children].forEach((c, i) => Thumbs.paint(c, 'w' + pics[i]));
      return d;
    }
    const d = el('i', {class: cls});
    if (pics.length) Thumbs.paint(d, 'w' + pics[0]);
    return d;
  }
  function face(ch) { const d = el('i', {class: 'face', title: CHARACTERS[ch] || ''}); Thumbs.paint(d, 'c' + (ch || 0)); return d; }

  // ---------------------------------------------------------------- connection
  const net = Module.net = {ws: null, ip: 0, ipStr: '', inbox: [], leftIps: [], room: null, members: [], connected: false, name: ''};
  let pending = null, onState = () => updateBadge(), roomsCb = () => {};
  const chatLog = [];
  const myChar = () => { const c = Number(store.get('pmwr-char', '0')); return c >= 0 && c < CHARACTERS.length ? c : 0; };

  function wsUrl() {
    const custom = params.get('server');
    if (custom) return custom;
    return (location.protocol === 'https:' ? 'wss://' : 'ws://') + location.host + '/ws';
  }
  const playerName = () => params.get('name') || store.get('pmwr-name', '') || 'Player';

  function connect() {
    if (net.ws && net.ws.readyState <= 1) return net.ws.readyState === 1 ? Promise.resolve() : new Promise((res, rej) => {
      net.ws.addEventListener('open', res, {once: true});
      net.ws.addEventListener('error', () => rej(new Error('Could not connect to the online server.')), {once: true});
    });
    return new Promise((resolve, reject) => {
      const ws = new WebSocket(wsUrl());
      ws.binaryType = 'arraybuffer';
      net.ws = ws;
      ws.onopen = () => { net.connected = true; net.name = playerName(); send({t: 'hello', name: net.name, char: myChar()}); resolve(); };
      ws.onerror = () => reject(new Error('Could not connect to the online server.'));
      ws.onclose = () => {
        const wasInRoom = !!net.room;
        net.connected = false;
        net.room = null;
        net.ip = 0;
        if (wasInRoom) {
          toast('Lost connection to the online server.');
          if (window.gameStarted) Module.onMessageBox('The connection to the online server dropped. Reload the page to get back to your room.', 'Online');
        }
        onState();
      };
      ws.onmessage = ev => {
        if (typeof ev.data !== 'string') { net.inbox.push(new Uint8Array(ev.data)); return; }
        handle(JSON.parse(ev.data));
      };
    });
  }

  const readyIps = new Map(); /* owner's lobby id -> Set of players whose game is ready in it */
  const failedIps = new Set();
  function handle(m) {
    switch (m.t) {
      case 'rooms': roomsCb(m.rooms); break;
      case 'joined':
        net.ip = ipToInt(m.ip);
        net.ipStr = m.ip;
        net.room = m.room;
        net.members = m.members;
        net.inbox.length = 0;
        session.set('pmwr-room', {code: m.room.code, token: m.token});
        if (pending) { pending.resolve(); pending = null; }
        onState();
        break;
      case 'members': net.room = m.room; net.members = m.members; onState(); cover.update(); break;
      case 'left': net.leftIps.push(ipToInt(m.ip)); addChat(null, m.name + ' left the room'); break;
      case 'chat': addChat(m.name, m.text); break;
      case 'start': onStart(m); break;
      case 'hostReady': hostReadyVer = Math.max(hostReadyVer, m.ver || 0); onState(); break;
      case 'matchReady':
        if (!readyIps.has(m.ver)) readyIps.set(m.ver, new Set());
        readyIps.get(m.ver).add(m.ip);
        onState();
        cover.update();
        break;
      case 'matchFailed': /* the owner stops waiting for that player */
        failedIps.add(m.ip);
        addChat(null, m.name + ' could not get into the match' + (m.msg ? ' (' + m.msg + ')' : ''));
        break;
      case 'pong': ping.pong(m.id); break;
      case 'go': if (stage.wantStart && !stage.playing) reveal(); break;
      case 'needLobby': /* someone cannot see our match in their list: open a fresh one (at most every 25 s) */
        if (isOwner() && !stage.playing && Date.now() - stage.recreatedAt > 25000) { stage.lobbyFor = ''; t('lobby asked for by ' + m.name); }
        break;
      case 'kicked':
        session.del('pmwr-room');
        net.room = null;
        net.ip = 0;
        toast('The room owner removed you from the room.');
        if (window.gameStarted) Module.onMessageBox('The room owner removed you from the room.', 'Online');
        onState();
        break;
      case 'error':
        if (pending) { pending.reject(new Error(m.msg)); pending = null; }
        else toast(m.msg);
        break;
    }
  }

  function send(obj) { if (net.ws && net.ws.readyState === 1) net.ws.send(JSON.stringify(obj)); }
  function request(obj) { return new Promise((resolve, reject) => { pending = {resolve, reject}; send(obj); }); }
  const isOwner = () => !!net.room && net.members.some(m => m.owner && m.ip === net.ipStr);

  // ---------------------------------------------------------------- small UI helpers
  function toast(t) {
    const d = el('div', {class: 'toast'}, t);
    document.body.append(d);
    setTimeout(() => d.remove(), 4500);
  }
  const chatViews = new Set();
  function addChat(name, text) {
    chatLog.push([name, text]);
    if (chatLog.length > 200) chatLog.shift();
    for (const v of chatViews) appendChatLine(v, name, text);
  }
  function appendChatLine(box, name, text) {
    box.append(name ? el('div', {}, el('b', {}, name + ': '), text) : el('div', {class: 'sys'}, text));
    box.scrollTop = box.scrollHeight;
  }

  // ---------------------------------------------------------------- "starting match" screen
  // Shown from "Start match" until the race is on screen (normally a moment: the games are
  // already waiting in the match lobby).
  const cover = (() => {
    let root = null;
    function build() {
      root = el('div', {id: 'matchcover'}, el('div', {class: 'card'},
        el('div', {class: 'ctitle'}, 'Starting match'),
        el('div', {class: 'ctrack'}), el('div', {class: 'cdesc'}),
        el('ul', {class: 'cplayers'}),
        el('div', {class: 'lbar'}, el('div', {class: 'lfill'})),
        el('div', {class: 'cstep'}), el('div', {class: 'cbtns'})));
      document.body.append(root);
    }
    const q = s => root.querySelector(s);
    const visible = () => !!root && root.style.display === 'flex';
    return {
      get visible() { return visible(); },
      show(s) {
        if (!root) build();
        const pics = picsOf(s || defaultSettings());
        const pic = q('.ctrack');
        pic.style.backgroundImage = '';
        if (pics.length) Thumbs.get('w' + pics[0]).then(u => { if (u) pic.style.backgroundImage = `url(${u})`; });
        q('.cdesc').textContent = describe(s || defaultSettings());
        q('.cbtns').replaceChildren(el('button', {onclick: () => { stage.wantStart = false; this.hide(); }}, 'Back to the room'));
        root.style.display = 'flex';
        this.update();
      },
      update() {
        if (!visible() || !net.room) return;
        q('.cstep').textContent = stage.status;
        const ready = readyOf(isOwner() ? stage.myVer : hostVer());
        q('.cplayers').replaceChildren(...net.members.filter(m => !m.away).map(m => el('li', {class: ready.has(m.ip) ? 'ready' : ''},
          face(m.char), (m.owner ? '★ ' : '') + m.name, el('small', {}, ready.has(m.ip) ? 'ready' : 'getting ready…'))));
      },
      hide() { if (root) root.style.display = 'none'; },
    };
  })();

  // ---------------------------------------------------------------- game driver
  // While the players are in the web room, each game boots behind it (muted, input blocked) and
  // the driver gets it ready through the game's own NETWORK PLAY menus: the owner's game keeps a
  // match lobby open with the room's settings, the others join it, and everybody's character is
  // picked and confirmed. "Start match" then only has to press START GAME.
  // Addresses in the recompiled game (PMR.exe), found by observing the menus.
  const A_SCREEN = 0x5f7ea0;   // index of the active interface screen
  const A_CHAR = 0x525814;     // characters picked in the match lobby (+ 1), one int per lobby slot
  const A_NAMES = 0x5258e0;    // player names of the lobby slots, 0x80 bytes each
  const ST = 0x80ac38;         // network game settings being edited by the host
  const SCR = {intro: 0, title: 1, main: 8, settings: 9, lobby: 11, netlist: 12, details: 19};
  const KEY = {enter: 0x1c, esc: 0x01, up: 0xc8, down: 0xd0, left: 0xcb, right: 0xcd, refresh: 0x13 /* R: Interface Network Refresh */};
  const m8 = a => Module.HEAPU8[a];
  const m32 = a => Module.HEAP32[a >> 2];
  const screen = () => m32(A_SCREEN);

  const trace = window.__drv = [];
  const t = msg => { trace.push(((performance.now() / 1000) | 0) + 's scr=' + screen() + ' ' + msg); if (trace.length > 300) trace.shift(); };
  const stage = {
    running: false, playing: false,   /* playing: the match runs and the game is on screen */
    lobbyFor: '', myVer: 0,           /* owner: settings and id of the match lobby its game has open */
    joinedVer: 0,                     /* others: the owner's lobby their game is in */
    isReady: false, readyChar: -1, readyVer: 0,
    wantStart: false, wantStartAt: 0, status: '', recreatedAt: 0,
  };
  class Restage extends Error {}
  class Fatal extends Error {} /* the game is somewhere the driver cannot safely leave */
  async function key(k, hold = 70, gap = 170) {
    if (!net.room) throw new Restage('left the room');
    while (window.__pauseDriver) await sleep(200); /* debugging aid */
    t('key ' + k);
    const q = Module.PM.events;
    q.push(1, KEY[k], 1, 0);
    await sleep(hold);
    q.push(1, KEY[k], 0, 0);
    await sleep(gap);
  }
  async function until(fn, ms) {
    const t0 = Date.now();
    while (Date.now() - t0 < ms) { if (fn()) return true; await sleep(80); }
    return fn();
  }
  function setStatus(s) { if (stage.status !== s) { stage.status = s; onState(); cover.update(); } }

  /* main menu -> NETWORK PLAY. The main menu always opens with its cursor on RACE, and NETWORK
   * PLAY is 2 items below (the last item is EXIT TO WINDOWS, so Enter is never pressed blindly). */
  async function mainToNetwork() {
    await sleep(900); /* the menu ignores input while it slides in */
    await key('down', 80, 450);
    await key('down', 80, 450);
    await key('enter');
    if (!await until(() => screen() === SCR.netlist, 6000)) throw new Fatal('could not open NETWORK PLAY');
    await sleep(400);
  }

  /* to the network game list, from boot or from any network screen */
  async function toNetworkList() {
    for (let i = 0; i < 400 && (screen() === SCR.title || screen() === SCR.intro); i++) {
      const s = screen();
      await key('enter'); /* also skips the intro movies */
      await until(() => screen() !== s, 1200);
    }
    for (let i = 0; i < 10; i++) {
      const s = screen();
      if (s === SCR.netlist) return;
      if (s === SCR.main) return mainToNetwork();
      if (![SCR.settings, SCR.details, SCR.lobby].includes(s)) { await sleep(400); continue; }
      await key('esc');
      await until(() => screen() !== s, 2500);
      await sleep(300);
    }
    throw new Fatal('lost track of the game menus');
  }

  /* searches the network again: the list only searches when it opens */
  async function reopenNetworkList() {
    await toNetworkList();
    await key('esc');
    if (!await until(() => screen() === SCR.main, 4000)) throw new Fatal('could not leave the network list');
    await mainToNetwork();
  }

  async function adjust(read, target, cyclic) {
    for (let i = 0; i < 24; i++) {
      const v = read();
      if (v === target) return true;
      await key(cyclic || v < target ? 'right' : 'left', 60, 110);
      if (!cyclic && read() === v) { await sleep(120); if (read() === v) return read() === target; } /* end of the list */
    }
    return read() === target;
  }

  /* owner: open a match lobby with the room's settings (from wherever the game is) */
  async function hostMatch(s) {
    const mode = modeOf(s.mode);
    stage.isReady = false;
    await toNetworkList();
    await key('enter'); /* Create Network Game */
    if (!await until(() => screen() === SCR.settings, 8000)) throw new Error('the match setup screen did not open');
    await sleep(500);
    await adjust(() => m8(ST + 4), s.maxPlayers, false);
    await key('down');
    await adjust(() => m8(ST + 3), s.cpu, false);
    await key('down');
    if (!await adjust(() => m8(ST + 5), mode.id, true)) throw new Error('could not select the game mode');
    await sleep(250);
    if (mode.kind === 'circuit') {
      await key('down');
      if (!await adjust(() => m8(ST + 6), s.world, true)) throw new Error('could not select the cup');
    } else {
      if (mode.opt) {
        await key('down');
        const o = mode.opt;
        await adjust(o.size === 1 ? () => m8(o.addr) : () => m32(o.addr), s.option || o.def, false);
      }
      await key('down');
      if (!await adjust(() => m8(ST + 0xc), s.world, true)) throw new Error('could not select the track');
    }
    await sleep(200);
    await key('enter');
    if (!await until(() => screen() === SCR.lobby, 8000)) throw new Error('the match lobby did not open');
    await sleep(700);
  }

  /* others: join the owner's match lobby */
  async function joinMatch() {
    stage.isReady = false;
    for (let tries = 0; tries < 6; tries++) {
      if (screen() === SCR.lobby) return;
      if (tries) await reopenNetworkList(); else await toNetworkList();
      await sleep(4500);    /* "Searching for Games…" lists the owner's game after a few seconds */
      await key('down');
      await key('enter');
      await until(() => screen() !== SCR.netlist, 3000);
      const s = screen();
      if (s === SCR.details) {
        for (let i = 0; i < 3 && screen() === SCR.details; i++) {
          await sleep(1200); /* the screen ignores input while it slides in */
          await key('enter'); /* JOIN GAME */
          if (await until(() => screen() === SCR.lobby, 5000)) { await sleep(500); return; }
        }
      } else if (s === SCR.settings) { /* the list was still empty: we opened "Create"; go back */
        if (tries >= 1) send({t: 'needLobby'});
        await sleep(1200); /* the screen ignores input while it slides in */
        await key('esc');
        await until(() => screen() === SCR.netlist, 4000);
        await sleep(800);
      }
    }
    throw new Error("the owner's match did not show up");
  }

  /* in the match lobby: Left/Right change the character, Enter confirms ("READY!"), Esc un-confirms */
  function mySlot() {
    const name = net.name || playerName();
    for (let k = 0; k < 8; k++) {
      const b = Module.HEAPU8.subarray(A_NAMES + k * 0x80, A_NAMES + (k + 1) * 0x80);
      let str = '';
      for (const c of b) str += c >= 32 && c < 127 ? String.fromCharCode(c) : '|';
      if (str.split('|').includes(name)) return k;
    }
    return 0;
  }
  async function ensureReady(ch, ver) {
    if (screen() !== SCR.lobby) return;
    if (!stage.isReady || stage.readyChar !== ch) {
      if (stage.isReady) { await key('esc'); await sleep(300); stage.isReady = false; }
      if (screen() !== SCR.lobby) return;
      const want = ch + 1, slot = mySlot(), cur = () => m8(A_CHAR + 4 * slot);
      /* one step at a time: held or quick presses skip characters */
      /* (a joiner's own choice only shows up after a round trip through the owner's game) */
      for (let i = 0; i < 20 && cur() !== want; i++) {
        const before = cur();
        await key('right', 80, 150);
        await until(() => cur() !== before, 3000);
        await sleep(250);
      }
      t('slot ' + slot + ' character ' + cur() + ' wanted ' + want);
      await key('enter');
      await sleep(250);
      stage.isReady = true;
      stage.readyChar = ch;
      stage.readyVer = 0;
    }
    if (stage.readyVer !== ver) { stage.readyVer = ver; send({t: 'matchReady', ver}); }
  }

  const hostVer = () => Math.max(hostReadyVer, (net.room && net.room.hostReady) || 0);
  function readyOf(ver) { return readyIps.get(ver) || new Set(); }
  function everyoneReady(ver) {
    const ready = readyOf(ver);
    return net.members.filter(m => !m.away && !failedIps.has(m.ip)).every(m => ready.has(m.ip));
  }
  const myCharacter = () => { const me = net.members.find(m => m.ip === net.ipStr); return me && Number.isInteger(me.char) ? me.char : myChar(); };

  async function stageStep() {
    if (isOwner()) {
      const want = JSON.stringify(net.room.settings || defaultSettings());
      if (screen() !== SCR.lobby || stage.lobbyFor !== want) {
        setStatus(stage.lobbyFor ? 'Updating the match…' : 'Setting up the match…');
        stage.lobbyFor = '';
        await hostMatch(JSON.parse(want));
        stage.lobbyFor = want;
        stage.myVer = stage.recreatedAt = Date.now();
        send({t: 'hostReady', ver: stage.myVer});
      }
      await ensureReady(myCharacter(), stage.myVer);
      if (stage.wantStart) {
        const waited = Date.now() - stage.wantStartAt;
        if (everyoneReady(stage.myVer) || waited > 60000) {
          setStatus('Starting…');
          await key('enter'); /* START GAME (shown once every player in the lobby is ready) */
          await sleep(600);
          send({t: 'go'});
          reveal();
          return;
        } else setStatus('Waiting for the other players…');
      } else setStatus(everyoneReady(stage.myVer) ? 'Ready' : 'Waiting for the other players…');
      return;
    }
    const hv = hostVer();
    if (stage.wantStart && stage.isReady && screen() !== SCR.lobby) { reveal(); return; } /* the owner started */
    if (!hv || (net.room.playing && !stage.wantStart)) {
      setStatus(net.room.playing ? 'A match is in progress' : 'Waiting for the room owner\'s game…');
      if (![SCR.netlist, SCR.lobby].includes(screen())) await toNetworkList();
      return;
    }
    if (screen() !== SCR.lobby) {
      setStatus('Joining the room owner\'s match…');
      await joinMatch();
      stage.joinedVer = hv;
    } else if (stage.joinedVer !== hv) {
      /* the owner opened a new lobby (settings changed): see whether this game was dropped */
      await sleep(2500);
      if (screen() !== SCR.lobby) return;
      stage.joinedVer = hv;
    }
    await ensureReady(myCharacter(), hv);
    setStatus(stage.wantStart ? 'Waiting for the room owner to start…' : 'Ready');
  }

  let muteTimer = 0;
  function mute(on) {
    clearInterval(muteTimer);
    const set = v => { const pm = Module.PM; if (pm && pm.master) pm.master.gain.value = v; };
    if (on) { set(0); muteTimer = setInterval(() => set(0), 250); } else set(1);
  }

  /* boots the game behind the room and keeps it ready until the match starts */
  async function startStaging() {
    if (stage.running || stage.playing || !net.room) return;
    if (!window.gameStarted) window.Online.startGame(['-network'], true);
    else if (!window.gameHidden) return; /* a match already ran on this page */
    stage.running = true;
    Module.inputBlocked = () => !stage.playing || !!ingame;
    mute(true);
    setStatus('Loading the game…');
    try {
      await until(() => Module.PM && Module.HEAP32 && screen() >= 0, 180000);
      let errors = 0;
      while (net.room && !stage.playing) {
        try { await stageStep(); errors = 0; }
        catch (e) {
          if (e instanceof Restage) break;
          t('error ' + e.message);
          if (e instanceof Fatal) {
            /* start the game over (the page keeps the room seat), at most a couple of times */
            const n = (session.get('pmwr-restarts') || 0) + 1;
            session.set('pmwr-restarts', n);
            if (n <= 2) { setStatus('Restarting the game…'); await sleep(800); location.reload(); return; }
            setStatus('Could not set the match up: ' + e.message + '. Reload the page to try again.');
            return;
          }
          if (++errors >= 3) setStatus('Having trouble: ' + e.message + ' (retrying)');
          stage.lobbyFor = '';
          await sleep(1500);
        }
        await sleep(200);
      }
    } finally { stage.running = false; }
  }

  function reveal() {
    session.del('pmwr-restarts');
    stage.playing = true;
    stage.wantStart = false;
    cover.hide();
    mute(false);
    window.Online.showGame();
    updateBadge();
  }

  let startHandled = 0, hostReadyVer = 0;
  function onStart(m) {
    if (m.seq <= startHandled) return;
    startHandled = m.seq;
    if (stage.playing) {
      /* a new match after one already ran: restart the page (keeps the room seat) so every game
       * starts from the same point */
      session.set('pmwr-autostart', {seq: m.seq});
      location.reload();
      return;
    }
    stage.wantStart = true;
    stage.wantStartAt = Date.now();
    cover.show(m.settings);
    startStaging();
  }

  // ---------------------------------------------------------------- views
  function settingsForm() {
    const wrap = el('div', {class: 'settings'});
    function render() {
      const s = net.room.settings || defaultSettings();
      const owner = isOwner();
      const mode = modeOf(s.mode);
      const set = fn => { const ns = {...s}; fn(ns); send({t: 'settings', settings: ns}); };
      const modes = el('div', {class: 'modes'}, MODES.map(m => el('button', {class: m.id === s.mode ? 'sel' : '', disabled: !owner,
        onclick: () => set(ns => { ns.mode = m.id; ns.world = worldsFor(m)[0][0]; ns.option = m.opt ? m.opt.def : 0; })}, m.short)));
      const tracks = el('div', {class: 'tracks'}, worldsFor(mode).map(([v, n, pics]) => el('button', {class: 'track' + (v === s.world ? ' sel' : ''),
        disabled: !owner && v !== s.world, title: n, onclick: () => owner && set(ns => { ns.world = v; })}, trackPic(pics, 'pic'), el('span', {}, n))));
      if (!owner) [...tracks.children].forEach(b => { if (!b.classList.contains('sel')) b.style.opacity = '.55'; });
      const sel = (label, value, options, apply) => {
        const select = el('select', {disabled: !owner}, options.map(([v, tx]) => el('option', {value: v, selected: v === value}, tx)));
        select.addEventListener('change', () => set(ns => apply(ns, Number(select.value))));
        return el('label', {class: 'field'}, el('span', {}, label), select);
      };
      const opts = [];
      if (mode.opt) opts.push(sel(mode.opt.label, s.option || mode.opt.def, mode.opt.values.map(v => [v, mode.opt.fmt(v)]), (ns, v) => { ns.option = v; }));
      opts.push(sel('Bots', s.cpu, CPU_LEVELS.map((n, i) => [i, n]), (ns, v) => { ns.cpu = v; }));
      opts.push(sel('Max players', s.maxPlayers, [[2, '2'], [3, '3'], [4, '4']], (ns, v) => { ns.maxPlayers = v; }));
      wrap.replaceChildren(modes, tracks, el('div', {class: 'opts'}, opts));
    }
    return {el: wrap, render};
  }

  function charPicker() {
    const wrap = el('div', {class: 'chars'});
    function render() {
      const me = net.members.find(m => m.ip === net.ipStr);
      const mine = me && Number.isInteger(me.char) ? me.char : myChar();
      const taken = new Map(net.members.filter(m => m !== me).map(m => [m.char, m.name]));
      wrap.replaceChildren(...CHARACTERS.map((n, i) => el('button', {class: 'char' + (i === mine ? ' sel' : ''),
        title: taken.has(i) ? n + ' (' + taken.get(i) + ')' : n, disabled: taken.has(i),
        onclick: () => { store.set('pmwr-char', String(i)); send({t: 'char', char: i}); }}, face(i), el('small', {}, taken.has(i) ? taken.get(i) : n))));
    }
    return {el: wrap, render};
  }

  function roomView(opts) {
    const titleEl = el('h3', {});
    const codeEl = el('span', {class: 'bigcode'});
    const membersEl = el('ul', {class: 'members'});
    const form = settingsForm();
    const chars = charPicker();
    const chatBox = el('div', {class: 'chat'});
    for (const [n, tx] of chatLog) appendChatLine(chatBox, n, tx);
    chatViews.add(chatBox);
    const chatIn = el('input', {maxlength: 200, placeholder: 'Message to the room…'});
    chatIn.addEventListener('keydown', e => {
      if (e.key === 'Enter' && chatIn.value.trim()) { send({t: 'chat', text: chatIn.value.trim()}); chatIn.value = ''; }
      e.stopPropagation();
    });
    chatIn.addEventListener('keyup', e => e.stopPropagation());
    const startBtn = el('button', {class: 'primary', onclick: () => send({t: 'start'})}, '▶ Start match');
    const waitEl = el('div', {class: 'hint'});
    const leave = () => {
      send({t: 'leave'}); session.del('pmwr-room'); net.room = null; net.ip = 0;
      if (window.gameStarted) { location.reload(); return; } /* stops the game that was getting ready */
      onState(); if (opts.onLeave) opts.onLeave();
    };
    const view = el('div', {class: 'roomview'},
      titleEl,
      el('div', {class: 'row'}, 'Code: ', codeEl,
        el('button', {class: 'small', onclick: () => { navigator.clipboard && navigator.clipboard.writeText(net.room.code); toast('Code copied'); }}, 'Copy'),
        el('button', {class: 'small', onclick: () => {
          const u = new URL(location.href); u.search = ''; u.searchParams.set('room', net.room.code);
          navigator.clipboard && navigator.clipboard.writeText(u.href); toast('Invite link copied');
        }}, 'Copy invite link')),
      el('h4', {}, 'Match'), form.el,
      el('div', {class: 'cols2'},
        el('div', {}, el('h4', {}, 'Players'), membersEl),
        el('div', {}, el('h4', {}, 'Your character'), chars.el)),
      el('h4', {}, 'Chat'), chatBox, chatIn,
      el('div', {class: 'row'}, startBtn, waitEl, el('button', {onclick: leave}, 'Leave room')),
    );
    function render() {
      if (!net.room) return;
      const owner = isOwner();
      titleEl.textContent = net.room.name + (net.room.public ? ' · public' : ' · private');
      codeEl.textContent = net.room.code;
      const ready = readyOf(owner ? stage.myVer : hostVer());
      const state = m => m.away ? ' · reconnecting…' : ready.has(m.ip) ? ' · ✔ ready' : ' · getting ready…';
      membersEl.replaceChildren(...net.members.map(m => el('li', {},
        face(m.char),
        el('div', {style: 'flex:1'}, (m.owner ? '★ ' : '') + m.name + (m.ip === net.ipStr ? ' (you)' : ''),
          el('small', {}, (CHARACTERS[m.char] || '') + state(m))),
        owner && m.ip !== net.ipStr ? el('button', {class: 'tiny', title: 'Remove from room', onclick: () => send({t: 'kick', ip: m.ip})}, '✕') : null)));
      form.render();
      chars.render();
      startBtn.style.display = owner ? '' : 'none';
      const present = net.members.filter(m => !m.away);
      const nReady = present.filter(m => ready.has(m.ip)).length;
      startBtn.textContent = '▶ Start match' + (!stage.playing && nReady < present.length ? ` (${nReady}/${present.length} ready)` : '');
      const mine = stage.playing ? '' : 'Your game: ' + (stage.status || 'loading…');
      waitEl.textContent = [mine, owner ? (net.members.length < 2 ? 'You can also start alone against bots.' : '')
                                        : (net.room.playing && !stage.playing ? 'A match is in progress.' : 'The owner starts the match.')].filter(Boolean).join(' · ');
    }
    return {el: view, render, dispose: () => chatViews.delete(chatBox)};
  }

  let built = null;
  function build(container, startGame, showGame) {
    window.Online.startGame = startGame;
    if (showGame) window.Online.showGame = showGame;
    if (built && built.isConnected) return; /* already open */
    const statusEl = el('div', {class: 'ostatus'});
    const status = tx => { statusEl.textContent = tx || ''; };
    const roomsEl = el('div', {class: 'rooms'});
    const nameIn = el('input', {maxlength: 20, value: playerName() === 'Player' ? '' : playerName(), placeholder: 'Your name'});
    const saveName = () => { store.set('pmwr-name', nameIn.value.trim()); net.name = playerName(); send({t: 'hello', name: net.name, char: myChar()}); };
    nameIn.addEventListener('change', saveName);
    const roomName = el('input', {maxlength: 20, placeholder: 'Room name (optional)'});
    const priv = el('input', {type: 'checkbox'});
    const codeIn = el('input', {maxlength: 6, placeholder: 'CODE', class: 'code'});
    codeIn.addEventListener('input', () => { codeIn.value = codeIn.value.toUpperCase().replace(/[^A-Z0-9]/g, ''); });

    async function enter(code) {
      try { saveName(); status('Joining…'); await connect(); await request({t: 'join', code}); status(''); }
      catch (e) { status(e.message); }
    }
    async function create() {
      try {
        saveName(); status('Creating room…'); await connect();
        let s = defaultSettings();
        try { s = {...s, ...JSON.parse(store.get('pmwr-last-settings', '{}'))}; } catch (e) {}
        await request({t: 'create', public: !priv.checked, name: roomName.value, settings: s});
        status('');
      } catch (e) { status(e.message); }
    }
    roomsCb = list => {
      roomsEl.replaceChildren();
      if (!list.length) { roomsEl.append(el('div', {class: 'empty'}, 'No public rooms open. Create one!')); return; }
      for (const r of list) {
        roomsEl.append(el('div', {class: 'room'},
          r.settings ? el('i', {class: 'rpic', style: 'width:72px;aspect-ratio:2/1;border-radius:6px;flex:none;background:#2a3042 center/cover no-repeat'}) : null,
          el('div', {class: 'rname', style: 'flex:1'}, r.name,
            el('small', {}, `${r.players}/${r.max} players · owner: ${r.owner}` + (r.playing ? ' · in a match' : '')),
            r.settings ? el('small', {}, describe(r.settings)) : null),
          el('button', {disabled: r.players >= r.max, onclick: () => enter(r.code)}, 'Join')));
        const pic = roomsEl.lastChild.querySelector('.rpic');
        if (pic) { const p = picsOf(r.settings); if (p.length) Thumbs.paint(pic, 'w' + p[0]); }
      }
    };

    const lobby = el('div', {class: 'lobby'},
      el('label', {class: 'row'}, 'Your name ', nameIn),
      el('h3', {}, 'Public rooms'),
      roomsEl,
      el('div', {class: 'cols'},
        el('div', {class: 'box'},
          el('h3', {}, 'Create a room'),
          roomName,
          el('label', {class: 'chk'}, priv, ' Private (only people with the code can join)'),
          el('button', {onclick: create}, 'Create room')),
        el('div', {class: 'box'},
          el('h3', {}, 'Join with a code'),
          codeIn,
          el('button', {onclick: () => { if (codeIn.value.length === 6) enter(codeIn.value); else status('The code has 6 characters.'); }}, 'Join'))),
    );
    const room = roomView({inGame: false});
    const panel = el('div', {class: 'online'},
      el('div', {class: 'tabs'}, el('button', {onclick: () => { panel.remove(); clearInterval(timer); room.dispose(); container.dispatchEvent(new Event('back')); }}, '← Back')),
      lobby, room.el, statusEl);
    container.append(panel);
    built = panel;

    onState = () => {
      lobby.style.display = net.room ? 'none' : '';
      room.el.style.display = net.room ? '' : 'none';
      room.render();
      updateBadge();
      if (net.room && isOwner() && net.room.settings) store.set('pmwr-last-settings', JSON.stringify(net.room.settings));
      if (net.room) startStaging(); /* the game gets the match ready while the players are in the room */
    };
    onState();
    const refresh = () => connect().then(() => send({t: 'list'})).catch(e => status(e.message));
    refresh();
    const timer = setInterval(() => { if (!net.room) refresh(); }, 3000);
    const auto = params.get('room');
    if (auto && !net.room) enter(auto.toUpperCase());
  }

  // ---------------------------------------------------------------- in-game: ping and room panel
  // Round trip to the online server, measured every 2 seconds while playing in a room.
  const ping = (() => {
    let node = null, sentAt = new Map(), seqNo = 0, last = 0;
    function show(ms) {
      if (!node) { node = el('div', {id: 'ping'}); document.body.append(node); }
      node.textContent = typeof ms === 'number' ? 'Ping ' + ms + ' ms' : 'Ping —';
      node.className = typeof ms !== 'number' || ms > 150 ? 'bad' : ms > 80 ? 'mid' : '';
    }
    setInterval(() => {
      const on = window.gameStarted && !window.gameHidden && net.room && net.connected;
      if (node) node.style.display = on && !cover.visible ? 'block' : 'none';
      if (!on) return;
      if (last && Date.now() - last > 6000) show('—');
      const id = ++seqNo;
      sentAt.set(id, performance.now());
      if (sentAt.size > 10) sentAt.delete(sentAt.keys().next().value);
      send({t: 'ping', id});
    }, 2000);
    return {
      pong(id) {
        const t0 = sentAt.get(id);
        if (t0 == null) return;
        sentAt.delete(id);
        last = Date.now();
        show(Math.round(performance.now() - t0));
        if (node) node.style.display = cover.visible ? 'none' : 'block';
      },
    };
  })();

  let badge, ingame;
  function updateBadge() {
    if (!window.gameStarted || window.gameHidden) { if (badge) badge.style.display = 'none'; return; }
    if (!badge) {
      badge = el('button', {class: 'netbadge', onclick: toggleRoomPanel});
      document.body.append(badge);
    }
    badge.style.display = net.room ? '' : 'none';
    if (net.room) badge.textContent = `Room ${net.room.code} · ${net.members.length} player${net.members.length > 1 ? 's' : ''} ☰`;
    if (ingame) ingame.render();
  }
  function toggleRoomPanel() {
    if (ingame) { ingame.wrap.remove(); ingame.dispose(); ingame = null; return; }
    const v = roomView({inGame: true, onLeave: toggleRoomPanel});
    const wrap = el('div', {class: 'online ingame'}, el('div', {class: 'tabs'}, el('button', {onclick: toggleRoomPanel}, '✕ Close (back to the game)')), v.el);
    document.body.append(wrap);
    ingame = {wrap, render: v.render, dispose: v.dispose};
    Module.inputBlocked = () => !stage.playing || !!ingame;
    v.render();
  }

  // ---------------------------------------------------------------- page load: rejoin / autostart
  async function resume() {
    const saved = session.get('pmwr-room');
    const auto = session.get('pmwr-autostart');
    if (!saved) return false;
    try {
      await connect();
      await request({t: 'join', code: saved.code, token: saved.token});
    } catch (e) { session.del('pmwr-room'); session.del('pmwr-autostart'); return false; }
    if (auto) { /* the owner started a new match: get ready and go straight into it */
      session.del('pmwr-autostart');
      startHandled = auto.seq;
      stage.wantStart = true;
      stage.wantStartAt = Date.now();
      cover.show(net.room.settings);
    }
    startStaging();
    return true;
  }

  window.Online = {build, net, updateBadge, resume, startGame: null};
})();
