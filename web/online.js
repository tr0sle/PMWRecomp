// Online rooms for the web port: lobby + room UI, WebSocket link to server/server.js,
// and a driver that takes everybody's game straight into the room's match.
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

  // ---------------------------------------------------------------- what the game offers online
  const fmtTime = ms => { const s = ms / 1000; return Math.floor(s / 60) + ':' + String(s % 60).padStart(2, '0'); };
  const TIME_VALUES = Array.from({length: 12}, (_, i) => (i + 1) * 30000);
  const RACE_WORLDS = ['Cloud Garden', 'Molten Mountain', 'Rolling Hills', 'Ghost Mansion', 'Canyon Crusade',
                       'Arctic Iceberg', 'Jungle Safari', 'Pirate Cove'];
  const ARENAS = ['Molten Mountain', 'Ghost Mansion', 'Canyon Crusade', 'Arctic Iceberg', "Spooky's Castle", 'Galactic Outpost'];
  const CUPS = [[1, 'Cherry Cup'], [2, 'Grape Cup']];
  const MODES = [
    {id: 0, name: 'Corrida', kind: 'race'},
    {id: 4, name: 'Circuito (copa de 4 pistas)', kind: 'circuit'},
    {id: 1, name: 'Batalha: Deathmatch', kind: 'battle', opt: {label: 'Abates para vencer', addr: 0x80ac48, size: 1, values: [5, 10, 15, 20, 25], def: 5, fmt: v => String(v)}},
    {id: 2, name: 'Batalha: Free For All', kind: 'battle', opt: {label: 'Tempo', addr: 0x80ac4c, size: 4, values: TIME_VALUES, def: 120000, fmt: fmtTime}},
    {id: 3, name: 'Batalha: Last Kart Driving', kind: 'battle', opt: {label: 'Vida', addr: 0x80ac50, size: 4, values: [100, 200, 300], def: 100, fmt: v => v + '%'}},
    {id: 6, name: 'Batalha: Binge', kind: 'battle', opt: {label: 'Tempo', addr: 0x80ac4c, size: 4, values: TIME_VALUES, def: 120000, fmt: fmtTime}},
    {id: 7, name: 'Batalha: Classic', kind: 'battle', opt: {label: 'Rodadas', addr: 0x80ac54, size: 1, values: [1, 2, 3], def: 1, fmt: v => String(v)}},
  ];
  const CPU_LEVELS = ['Sem CPUs', 'Fácil', 'Normal', 'Difícil', 'Pesadelo'];
  const modeOf = id => MODES.find(m => m.id === id) || MODES[0];
  const worldsFor = mode => mode.kind === 'race' ? RACE_WORLDS.map((n, i) => [i, n]) : mode.kind === 'battle' ? ARENAS.map((n, i) => [i, n]) : CUPS;
  const defaultSettings = () => ({mode: 0, world: 0, option: 0, cpu: 2, maxPlayers: 4});
  function describe(s) {
    const m = modeOf(s.mode);
    const w = worldsFor(m).find(([id]) => id === s.world);
    const parts = [m.name, w ? w[1] : '?'];
    if (m.opt) parts.push(m.opt.label + ': ' + m.opt.fmt(s.option || m.opt.def));
    parts.push(CPU_LEVELS[s.cpu] || '', 'até ' + s.maxPlayers + ' jogadores');
    return parts.filter(Boolean).join(' · ');
  }

  // ---------------------------------------------------------------- connection
  const net = Module.net = {ws: null, ip: 0, ipStr: '', inbox: [], leftIps: [], room: null, members: [], connected: false, name: ''};
  let pending = null, onState = () => updateBadge(), roomsCb = () => {};
  const chatLog = [];

  function wsUrl() {
    const custom = params.get('server');
    if (custom) return custom;
    return (location.protocol === 'https:' ? 'wss://' : 'ws://') + location.host + '/ws';
  }
  const playerName = () => params.get('name') || store.get('pmwr-name', '') || 'Jogador';

  function connect() {
    if (net.ws && net.ws.readyState <= 1) return net.ws.readyState === 1 ? Promise.resolve() : new Promise((res, rej) => {
      net.ws.addEventListener('open', res, {once: true});
      net.ws.addEventListener('error', () => rej(new Error('Não foi possível conectar ao servidor online.')), {once: true});
    });
    return new Promise((resolve, reject) => {
      const ws = new WebSocket(wsUrl());
      ws.binaryType = 'arraybuffer';
      net.ws = ws;
      ws.onopen = () => { net.connected = true; net.name = playerName(); send({t: 'hello', name: net.name}); resolve(); };
      ws.onerror = () => reject(new Error('Não foi possível conectar ao servidor online.'));
      ws.onclose = () => {
        const wasInRoom = !!net.room;
        net.connected = false;
        net.room = null;
        net.ip = 0;
        if (wasInRoom) {
          toast('Conexão com o servidor online perdida.');
          if (window.gameStarted) Module.onMessageBox('A conexão com o servidor online caiu. Recarregue a página para voltar à sala.', 'Online');
        }
        onState();
      };
      ws.onmessage = ev => {
        if (typeof ev.data !== 'string') { net.inbox.push(new Uint8Array(ev.data)); return; }
        handle(JSON.parse(ev.data));
      };
    });
  }

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
      case 'members': net.room = m.room; net.members = m.members; onState(); break;
      case 'left': net.leftIps.push(ipToInt(m.ip)); addChat(null, m.name + ' saiu da sala'); break;
      case 'chat': addChat(m.name, m.text); break;
      case 'start': onStart(m); break;
      case 'hostReady': hostReadySeq = Math.max(hostReadySeq, m.seq); break;
      case 'kicked':
        session.del('pmwr-room');
        net.room = null;
        net.ip = 0;
        toast('Você foi removido da sala pelo dono.');
        if (window.gameStarted) Module.onMessageBox('Você foi removido da sala pelo dono.', 'Online');
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

  // ---------------------------------------------------------------- game driver
  // Addresses in the recompiled game (PMR.exe), found by observing the menus.
  const A_SCREEN = 0x5f7ea0;   // index of the active interface screen
  const A_CURSOR = 0x5f0f08;   // selected item of the main menu (1 Race, 2 Hall of Fame, 3 Network Play ...)
  const ST = 0x80ac38;         // network game settings being edited by the host
  const SCR = {intro: 0, title: 1, main: 8, settings: 9, lobby: 11, netlist: 12, details: 19};
  const KEY = {enter: 0x1c, esc: 0x01, up: 0xc8, down: 0xd0, left: 0xcb, right: 0xcd, refresh: 0x13 /* R: Interface Network Refresh */};
  const m8 = a => Module.HEAPU8[a];
  const m32 = a => Module.HEAP32[a >> 2];
  const screen = () => m32(A_SCREEN);
  let driving = false, banner = null, cancelled = false, bannerGen = 0;

  const trace = window.__drv = [];
  const t = msg => { trace.push(((performance.now() / 1000) | 0) + 's scr=' + screen() + ' ' + msg); if (trace.length > 300) trace.shift(); };
  async function key(k) {
    if (cancelled) throw new Error('cancelado');
    t('key ' + k);
    const q = Module.PM.events;
    q.push(1, KEY[k], 1, 0);
    await sleep(100);
    q.push(1, KEY[k], 0, 0);
    await sleep(260);
  }
  async function until(fn, ms) {
    const t0 = Date.now();
    while (Date.now() - t0 < ms) { if (fn()) return true; await sleep(100); }
    return fn();
  }
  function showBanner(text, sub, cancellable) {
    if (!banner) { banner = el('div', {class: 'drivebanner'}); document.body.append(banner); }
    bannerGen++;
    banner.replaceChildren(...[el('div', {}, text), sub ? el('small', {}, sub) : null,
      cancellable ? el('button', {class: 'small', onclick: () => { cancelled = true; }}, 'Cancelar') : null].filter(Boolean));
    banner.style.display = '';
  }
  function hideBanner() { if (banner) banner.style.display = 'none'; }
  function hideBannerLater(ms) { const g = bannerGen; setTimeout(() => { if (g === bannerGen) hideBanner(); }, ms); }

  async function toMainMenu() {
    for (let i = 0; i < 240; i++) {
      const s = screen();
      if (s === SCR.main) return;
      if (s === SCR.title) {
        await key('enter');
        await until(() => screen() !== SCR.title, 4000);
      } else if (s === SCR.netlist || s === SCR.settings || s === SCR.details) {
        await key('esc'); /* back out of the network screens */
        await until(() => screen() !== s, 2500);
      } else await sleep(500);
    }
    throw new Error('o jogo não chegou ao menu principal');
  }

  /* The menu cursor lives in the interface's object pool, whose layout depends on the
   * screens opened so far, so it is located at run time: the value that moves 1 step
   * with Down and back with Up (5 items, wrapping). */
  async function findMenuCursor() {
    const lo = 0x5e0000 >> 2, hi = 0x620000 >> 2;
    const snap = () => Module.HEAP32.slice(lo, hi);
    const a = snap(); await key('down'); await sleep(250);
    const b = snap(); await key('up'); await sleep(250);
    const c = snap();
    const hits = [];
    for (let i = 0; i < a.length; i++) {
      const v = a[i];
      if (v >= 1 && v <= 5 && c[i] === v && b[i] === (v === 5 ? 1 : v + 1)) hits.push((i + lo) * 4);
    }
    return hits.length === 1 ? hits[0] : (hits.length ? hits[0] : 0);
  }

  async function toNetworkList() {
    for (let attempt = 0; attempt < 3; attempt++) {
      await toMainMenu();
      await sleep(800);
      const cur = await findMenuCursor();
      if (cur) {
        for (let i = 0; i < 6 && m32(cur) !== 3; i++) await key('down');
      } else if (attempt === 0) {
        await key('down'); await key('down'); /* fresh boot: the cursor starts on RACE */
      }
      await key('enter');
      if (await until(() => screen() === SCR.netlist, 8000)) { await sleep(600); return; }
      /* wrong item: go back to the main menu and try again */
      for (let i = 0; i < 4 && screen() !== SCR.main; i++) { await key('esc'); await sleep(900); }
    }
    throw new Error('não consegui abrir NETWORK PLAY');
  }

  async function adjust(read, target, cyclic) {
    for (let i = 0; i < 24; i++) {
      const v = read();
      if (v === target) return true;
      await key(cyclic || v < target ? 'right' : 'left');
      await sleep(150);
      if (!cyclic && read() === v) return read() === target; /* reached the end of the list */
    }
    return read() === target;
  }

  async function hostMatch(s) {
    const mode = modeOf(s.mode);
    showBanner('Criando a partida…', describe(s));
    await toNetworkList();
    await key('enter'); /* Create Network Game */
    if (!await until(() => screen() === SCR.settings, 10000)) throw new Error('a tela de criação de partida não abriu');
    await sleep(700);
    const warn = [];
    await adjust(() => m8(ST + 4), s.maxPlayers, false);
    await key('down');
    await adjust(() => m8(ST + 3), s.cpu, false);
    await key('down');
    if (!await adjust(() => m8(ST + 5), mode.id, true)) throw new Error('não foi possível escolher o modo');
    await sleep(400);
    if (mode.kind === 'circuit') {
      await key('down');
      if (!await adjust(() => m8(ST + 6), s.world, true)) warn.push('copa indisponível no save do dono');
    } else {
      if (mode.opt) {
        await key('down');
        const o = mode.opt;
        const rd = o.size === 1 ? () => m8(o.addr) : () => m32(o.addr);
        await adjust(rd, s.option || o.def, false);
      }
      await key('down');
      if (!await adjust(() => m8(ST + 0xc), s.world, true)) warn.push('pista indisponível no save do dono');
    }
    await sleep(300);
    await key('enter');
    if (!await until(() => screen() === SCR.lobby, 10000)) throw new Error('o lobby da partida não abriu');
    await sleep(1500);
    send({t: 'hostReady'});
    if (warn.length) send({t: 'chat', text: '(aviso: ' + warn.join(', ') + ')'});
  }

  async function joinMatch(seq) {
    showBanner('Procurando a partida do dono da sala…', null, true);
    await toNetworkList();
    /* wait until the owner's game has really been created (it is listed earlier, while still being set up) */
    const readyNow = () => hostReadySeq >= seq || (net.room && net.room.hostReady >= seq);
    if (seq && !await until(readyNow, 120000)) throw new Error('o dono ainda não abriu a partida');
    const t0 = Date.now();
    await sleep(1000);
    let told = false, tries = 0;
    while (Date.now() - t0 < 15 * 60000) {
      if (!told && Date.now() - t0 > 20000) {
        told = true;
        showBanner('A partida do dono ainda está em andamento.', 'Você entra automaticamente assim que ela terminar e o dono voltar ao lobby do jogo.', true);
      }
      const cur = screen();
      if (cur === SCR.lobby) return;
      if (cur === SCR.settings || cur === SCR.details) {
        /* back out of whatever opened (never confirm anything except JOIN GAME) */
        await key('esc');
        await until(() => screen() === SCR.netlist, 4000);
        continue;
      }
      if (cur !== SCR.netlist) { await toNetworkList(); continue; }
      if (tries++ > 0) {
        /* the list only searches when the screen opens: leave to the main menu and come back */
        await key('esc');
        await until(() => screen() === SCR.main, 5000);
        await sleep(800);
        await toNetworkList();
      }
      await sleep(3000);
      await key('down');
      await key('enter');
      await until(() => screen() !== SCR.netlist, 4000);
      const s = screen();
      if (s === SCR.details) {
        await sleep(600);
        await key('enter'); /* JOIN GAME */
        if (await until(() => screen() === SCR.lobby, 10000)) return;
        continue;
      }
      if (s === SCR.settings) { /* the list was still empty: we opened "Create"; go back and wait */
        await sleep(1500); /* the screen ignores input while it slides in */
        await key('esc');
        await until(() => screen() === SCR.netlist, 5000);
      }
      await sleep(2000);
    }
    throw new Error('a partida do dono não apareceu');
  }

  async function drive(asHost, s, seq) {
    if (driving) return;
    driving = true;
    cancelled = false;
    Module.inputBlocked = () => driving;
    if (window.Online.updateBadge) window.Online.updateBadge();
    try {
      await until(() => Module.PM && Module.HEAP32, 60000);
      if (asHost) await hostMatch(s); else await joinMatch(seq);
      showBanner('Na sala da partida!', 'Escolha seu personagem e confirme (Enter). Quando todos estiverem prontos, o dono aperta Enter em START GAME.');
      hideBannerLater(12000);
    } catch (e) {
      if (cancelled) { showBanner('Entrada automática cancelada.', 'Você pode usar o menu NETWORK PLAY do jogo ou o botão da sala.'); }
      else showBanner('Não foi possível entrar automaticamente: ' + e.message, 'Use o menu NETWORK PLAY do jogo para criar ou entrar na partida.');
      hideBannerLater(15000);
    } finally {
      driving = false;
      cancelled = false;
      updateBadge();
    }
  }

  let startHandled = 0, hostReadySeq = 0;
  function onStart(m) {
    if (m.seq <= startHandled) return;
    startHandled = m.seq;
    const asHost = m.host === net.ipStr;
    if (window.gameStarted) {
      /* a new match: restart the page (keeps the room seat) so every game starts from the same point */
      session.set('pmwr-autostart', {seq: m.seq, host: asHost, settings: m.settings});
      location.reload();
      return;
    }
    window.Online.startGame(['-network']);
    drive(asHost, m.settings, m.seq);
  }

  // ---------------------------------------------------------------- views
  function settingsForm(onChange) {
    const wrap = el('div', {class: 'settings'});
    function render() {
      const s = net.room.settings || defaultSettings();
      const owner = isOwner();
      const mode = modeOf(s.mode);
      const sel = (label, value, options, apply) => {
        const select = el('select', {disabled: !owner}, options.map(([v, t]) => el('option', {value: v, selected: v === value}, t)));
        select.addEventListener('change', () => { const ns = {...s}; apply(ns, Number(select.value)); send({t: 'settings', settings: ns}); });
        return el('label', {class: 'field'}, el('span', {}, label), select);
      };
      const rows = [
        sel('Modo', s.mode, MODES.map(m => [m.id, m.name]), (ns, v) => {
          ns.mode = v;
          const nm = modeOf(v);
          ns.world = worldsFor(nm)[0][0];
          ns.option = nm.opt ? nm.opt.def : 0;
        }),
        sel(mode.kind === 'circuit' ? 'Copa' : mode.kind === 'battle' ? 'Arena' : 'Pista', s.world, worldsFor(mode), (ns, v) => { ns.world = v; }),
      ];
      if (mode.opt) rows.push(sel(mode.opt.label, s.option || mode.opt.def, mode.opt.values.map(v => [v, mode.opt.fmt(v)]), (ns, v) => { ns.option = v; }));
      rows.push(sel('CPUs', s.cpu, CPU_LEVELS.map((n, i) => [i, n]), (ns, v) => { ns.cpu = v; }));
      rows.push(sel('Máx. jogadores', s.maxPlayers, [[2, '2'], [3, '3'], [4, '4']], (ns, v) => { ns.maxPlayers = v; }));
      wrap.replaceChildren(...rows);
      if (onChange) onChange();
    }
    return {el: wrap, render};
  }

  function roomView(opts) {
    const titleEl = el('h3', {});
    const codeEl = el('span', {class: 'bigcode'});
    const membersEl = el('ul', {class: 'members'});
    const form = settingsForm();
    const chatBox = el('div', {class: 'chat'});
    for (const [n, t] of chatLog) appendChatLine(chatBox, n, t);
    chatViews.add(chatBox);
    const chatIn = el('input', {maxlength: 200, placeholder: 'Mensagem para a sala…'});
    chatIn.addEventListener('keydown', e => {
      if (e.key === 'Enter' && chatIn.value.trim()) { send({t: 'chat', text: chatIn.value.trim()}); chatIn.value = ''; }
      e.stopPropagation();
    });
    chatIn.addEventListener('keyup', e => e.stopPropagation());
    const startBtn = el('button', {class: 'primary', onclick: () => send({t: 'start'})}, 'Iniciar partida');
    const waitEl = el('div', {class: 'hint'});
    const joinBtn = el('button', {class: opts.inGame ? 'small' : 'primary', onclick: () => {
      if (window.gameStarted) { if (ingame) toggleRoomPanel(); }
      else window.Online.startGame(['-network']);
      drive(false, null, 0);
    }}, 'Entrar na partida do dono');
    const view = el('div', {class: 'roomview'},
      titleEl,
      el('div', {class: 'row'}, 'Código: ', codeEl,
        el('button', {class: 'small', onclick: () => { navigator.clipboard && navigator.clipboard.writeText(net.room.code); toast('Código copiado'); }}, 'Copiar'),
        el('button', {class: 'small', onclick: () => {
          const u = new URL(location.href); u.search = ''; u.searchParams.set('room', net.room.code);
          navigator.clipboard && navigator.clipboard.writeText(u.href); toast('Link de convite copiado');
        }}, 'Copiar link de convite')),
      el('div', {class: 'cols2'},
        el('div', {}, el('h4', {}, 'Jogadores'), membersEl),
        el('div', {}, el('h4', {}, 'Partida'), form.el)),
      el('h4', {}, 'Chat'), chatBox, chatIn,
      el('div', {class: 'row'}, startBtn, waitEl, joinBtn,
        el('button', {onclick: () => { send({t: 'leave'}); session.del('pmwr-room'); net.room = null; net.ip = 0; onState(); if (opts.onLeave) opts.onLeave(); }}, 'Sair da sala')),
    );
    function render() {
      if (!net.room) return;
      const owner = isOwner();
      titleEl.textContent = net.room.name + (net.room.public ? ' · pública' : ' · privada');
      codeEl.textContent = net.room.code;
      membersEl.replaceChildren(...net.members.map(m => el('li', {},
        (m.owner ? '★ ' : '') + m.name + (m.ip === net.ipStr ? ' (você)' : '') + (m.away ? ' — reconectando…' : ''),
        owner && m.ip !== net.ipStr ? el('button', {class: 'tiny', title: 'Remover da sala', onclick: () => send({t: 'kick', ip: m.ip})}, '✕') : null)));
      form.render();
      startBtn.style.display = owner ? '' : 'none';
      startBtn.disabled = net.members.length < 1;
      const inMatch = window.gameStarted && !driving;
      waitEl.textContent = owner ? (net.members.length < 2 ? 'Aguardando mais jogadores (você pode iniciar sozinho com CPUs).' : '')
                                 : net.room.playing ? (inMatch ? '' : 'Uma partida está em andamento: você pode entrar nela.') : 'Aguardando o dono iniciar a partida…';
      joinBtn.style.display = !owner && net.room.playing && !driving ? '' : 'none';
    }
    return {el: view, render, dispose: () => chatViews.delete(chatBox)};
  }

  let built = null;
  function build(container, startGame) {
    window.Online.startGame = startGame;
    if (built && built.isConnected) return; /* already open */
    const statusEl = el('div', {class: 'ostatus'});
    const status = t => { statusEl.textContent = t || ''; };
    const roomsEl = el('div', {class: 'rooms'});
    const nameIn = el('input', {maxlength: 20, value: playerName() === 'Jogador' ? '' : playerName(), placeholder: 'Seu nome'});
    const saveName = () => { store.set('pmwr-name', nameIn.value.trim()); net.name = playerName(); send({t: 'hello', name: net.name}); };
    nameIn.addEventListener('change', saveName);
    const roomName = el('input', {maxlength: 20, placeholder: 'Nome da sala (opcional)'});
    const priv = el('input', {type: 'checkbox'});
    const codeIn = el('input', {maxlength: 6, placeholder: 'CÓDIGO', class: 'code'});
    codeIn.addEventListener('input', () => { codeIn.value = codeIn.value.toUpperCase().replace(/[^A-Z0-9]/g, ''); });

    async function enter(code) {
      try { saveName(); status('Entrando…'); await connect(); await request({t: 'join', code}); status(''); }
      catch (e) { status(e.message); }
    }
    async function create() {
      try {
        saveName(); status('Criando sala…'); await connect();
        await request({t: 'create', public: !priv.checked, name: roomName.value, settings: defaultSettings()});
        status('');
      } catch (e) { status(e.message); }
    }
    roomsCb = list => {
      roomsEl.replaceChildren();
      if (!list.length) { roomsEl.append(el('div', {class: 'empty'}, 'Nenhuma sala pública aberta. Crie uma!')); return; }
      for (const r of list) {
        roomsEl.append(el('div', {class: 'room'},
          el('div', {class: 'rname'}, r.name,
            el('small', {}, `${r.players}/${r.max} jogadores · dono: ${r.owner}` + (r.playing ? ' · em partida' : '')),
            r.settings ? el('small', {}, describe(r.settings)) : null),
          el('button', {disabled: r.players >= r.max, onclick: () => enter(r.code)}, 'Entrar')));
      }
    };

    const lobby = el('div', {class: 'lobby'},
      el('label', {class: 'row'}, 'Seu nome ', nameIn),
      el('h3', {}, 'Salas públicas'),
      roomsEl,
      el('div', {class: 'cols'},
        el('div', {class: 'box'},
          el('h3', {}, 'Criar sala'),
          roomName,
          el('label', {class: 'chk'}, priv, ' Privada (só entra quem tiver o código)'),
          el('button', {onclick: create}, 'Criar sala')),
        el('div', {class: 'box'},
          el('h3', {}, 'Entrar com código'),
          codeIn,
          el('button', {onclick: () => { if (codeIn.value.length === 6) enter(codeIn.value); else status('O código tem 6 caracteres.'); }}, 'Entrar'))),
    );
    const room = roomView({inGame: false});
    const panel = el('div', {class: 'online'},
      el('div', {class: 'tabs'}, el('button', {onclick: () => { panel.remove(); clearInterval(timer); room.dispose(); container.dispatchEvent(new Event('back')); }}, '← Voltar')),
      lobby, room.el, statusEl);
    container.append(panel);
    built = panel;

    onState = () => {
      lobby.style.display = net.room ? 'none' : '';
      room.el.style.display = net.room ? '' : 'none';
      room.render();
      updateBadge();
    };
    onState();
    const refresh = () => connect().then(() => send({t: 'list'})).catch(e => status(e.message));
    refresh();
    const timer = setInterval(() => { if (!net.room) refresh(); }, 3000);
    const auto = params.get('room');
    if (auto && !net.room) enter(auto.toUpperCase());
  }

  // ---------------------------------------------------------------- in-game room panel
  let badge, ingame;
  function updateBadge() {
    if (!window.gameStarted) return;
    if (!badge) {
      badge = el('button', {class: 'netbadge', onclick: toggleRoomPanel});
      document.body.append(badge);
    }
    badge.style.display = net.room ? '' : 'none';
    if (net.room) badge.textContent = `Sala ${net.room.code} · ${net.members.length} jogador${net.members.length > 1 ? 'es' : ''} ☰`;
    if (ingame) ingame.render();
  }
  function toggleRoomPanel() {
    if (ingame) { ingame.wrap.remove(); ingame.dispose(); ingame = null; Module.inputBlocked = () => driving; return; }
    const v = roomView({inGame: true, onLeave: toggleRoomPanel});
    const wrap = el('div', {class: 'online ingame'}, el('div', {class: 'tabs'}, el('button', {onclick: toggleRoomPanel}, '✕ Fechar (voltar ao jogo)')), v.el);
    document.body.append(wrap);
    ingame = {wrap, render: v.render, dispose: v.dispose};
    Module.inputBlocked = () => true;
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
    if (auto) {
      session.del('pmwr-autostart');
      startHandled = auto.seq;
      window.Online.startGame(['-network']);
      drive(auto.host, auto.settings, auto.seq);
    }
    return true;
  }

  window.Online = {build, net, updateBadge, resume, startGame: null};
})();
