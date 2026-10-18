// Host services for the recompiled game (declared in runtime/js.h).
// Async functions suspend the wasm stack through JSPI.

addToLibrary({
  $PM__deps: ['$UTF8ToString'],
  $PM: {
    files: null,        // lowercased game path -> {name, size}
    dirs: null,         // lowercased directory paths (game data)
    cache: new Map(),   // lowercased game path -> Uint8Array (LRU by insertion order)
    cacheBytes: 0,
    saves: new Map(),   // lowercased "save/..." -> {name, data: Uint8Array}
    saveDirs: new Set(),
    fds: [],
    events: [],
    cursorVisible: true,
    gameW: 640, gameH: 480,
    audio: null,
    sndBufs: new Map(), sndNext: 1,
    chans: [],
    dbPromise: null,
    savePending: new Set(),
    saveTimer: 0,

    norm(p) { return p.replace(/\\/g, '/').replace(/^\.\//, '').replace(/\/+/g, '/').replace(/\/$/, '').toLowerCase(); },

    async fetchFile(key) {
      const hit = PM.cache.get(key);
      if (hit) { PM.cache.delete(key); PM.cache.set(key, hit); return hit; }
      const ent = PM.files.get(key);
      if (!ent) return null;
      const progress = (done, total) => Module.loadProgress && Module.loadProgress(ent.name, done, total);
      progress(0, ent.size);
      let data;
      try {
        if (ent.base === 'idb:') {
          /* the player's own copy of the game, stored in the browser (web/gamestore.js) */
          data = await window.GameStore.read(ent.name);
          if (!data) { console.error('missing game file', ent.name); return null; }
        } else {
          const r = await fetch((ent.base || Module.gameBase) + encodeURI(ent.name));
          if (!r.ok) { console.error('fetch failed', ent.name, r.status); return null; }
          /* stream the body to report progress (Content-Length is absent when compressed) */
          const total = ent.size || Number(r.headers.get('content-length')) || 0;
          const reader = r.body.getReader();
          let buf = new Uint8Array(total || 1 << 20), n = 0;
          for (;;) {
            const {done, value} = await reader.read();
            if (done) break;
            if (n + value.length > buf.length) { const nb = new Uint8Array(Math.max(buf.length * 2, n + value.length)); nb.set(buf.subarray(0, n)); buf = nb; }
            buf.set(value, n);
            n += value.length;
            progress(n, total);
          }
          data = n === buf.length ? buf : buf.slice(0, n);
        }
      } finally {
        Module.loadDone && Module.loadDone(ent.name);
      }
      const online = !!(Module.net && Module.net.room);
      for (const [file, off, from, to, onlineOnly] of Module.filePatches || []) {
        if (file !== key || (onlineOnly && !online)) continue;
        if (from.every((b, i) => data[off + i] === b)) { data.set(to, off); console.log('patched', file, off.toString(16)); }
        else console.warn('patch does not match', file, off.toString(16));
      }
      PM.cache.set(key, data);
      PM.cacheBytes += data.length;
      const open = new Set(PM.fds.filter(f => f && f.key).map(f => f.key));
      for (const [k, v] of PM.cache) {
        if (PM.cacheBytes <= 384 * 1024 * 1024) break;
        if (k === key || open.has(k)) continue;
        PM.cache.delete(k);
        PM.cacheBytes -= v.length;
      }
      return data;
    },

    openDB() {
      if (!PM.dbPromise) PM.dbPromise = new Promise((res, rej) => {
        const rq = indexedDB.open('pmwr-saves', 1);
        rq.onupgradeneeded = () => rq.result.createObjectStore('files');
        rq.onsuccess = () => res(rq.result);
        rq.onerror = () => rej(rq.error);
      });
      return PM.dbPromise;
    },
    async loadSaves() {
      try {
        const db = await PM.openDB();
        await new Promise((res, rej) => {
          const tx = db.transaction('files', 'readonly');
          const st = tx.objectStore('files');
          const rq = st.openCursor();
          rq.onsuccess = () => {
            const c = rq.result;
            if (!c) return;
            const v = c.value;
            if (v && v.dir) PM.saveDirs.add(c.key);
            else if (v) PM.saves.set(c.key, {name: v.name, data: new Uint8Array(v.data)});
            c.continue();
          };
          tx.oncomplete = res;
          tx.onerror = () => rej(tx.error);
        });
      } catch (e) { console.warn('save storage unavailable', e); }
    },
    persist(key) {
      PM.savePending.add(key);
      if (PM.saveTimer) return;
      PM.saveTimer = setTimeout(async () => {
        PM.saveTimer = 0;
        const keys = [...PM.savePending];
        PM.savePending.clear();
        try {
          const db = await PM.openDB();
          const tx = db.transaction('files', 'readwrite');
          const st = tx.objectStore('files');
          for (const k of keys) {
            const s = PM.saves.get(k);
            if (s) st.put({name: s.name, data: s.data.slice().buffer}, k);
            else if (PM.saveDirs.has(k)) st.put({dir: true}, k);
            else st.delete(k);
          }
        } catch (e) { console.warn('save failed', e); }
      }, 200);
    },
    parentDirs(key) {
      const parts = key.split('/');
      for (let i = 1; i < parts.length; i++) {
        const d = parts.slice(0, i).join('/');
        if (d.startsWith('save') && !PM.saveDirs.has(d)) { PM.saveDirs.add(d); PM.persist(d); }
      }
    },
  },

  js_init__deps: ['$PM'],
  js_init: function () {
    Module.PM = PM; // for debugging from the console
    const m = Module.manifest;
    PM.files = new Map();
    PM.dirs = new Set(['']);
    for (const [name, size, base] of m) {
      const k = PM.norm(name);
      PM.files.set(k, {name, size, base});
      const parts = k.split('/');
      for (let i = 1; i < parts.length; i++) PM.dirs.add(parts.slice(0, i).join('/'));
    }
    for (const [k, v] of Module.savedFiles || []) PM.saves.set(k, v);
    for (const d of Module.savedDirs || []) PM.saveDirs.add(d);
    PM.saveDirs.add('save');
    const q = PM.events;
    const scan = Module.scanCodes;
    const gameKeys = new Set(['ArrowUp', 'ArrowDown', 'ArrowLeft', 'ArrowRight', 'Space', 'Tab', 'Backspace', 'Enter',
      'F1', 'F2', 'F3', 'F4', 'F5', 'F6', 'F7', 'F8', 'F9', 'F10', 'F11', 'F12', 'AltLeft', 'AltRight', 'ControlLeft', 'Escape']);
    const onKey = (e, down) => {
      if (Module.inputBlocked && Module.inputBlocked()) return;
      const sc = scan[e.code];
      if (sc !== undefined && !(down && e.repeat)) q.push(1, sc, down ? 1 : 0, 0);
      if (down && e.key && e.key.length === 1 && !e.ctrlKey && !e.metaKey) q.push(2, e.key.charCodeAt(0) & 0xff, 0, 0);
      else if (down && e.code === 'Backspace') q.push(2, 8, 0, 0);
      else if (down && e.code === 'Enter') q.push(2, 13, 0, 0);
      else if (down && e.code === 'Escape') q.push(2, 27, 0, 0);
      if (gameKeys.has(e.code) || sc !== undefined) e.preventDefault();
    };
    window.addEventListener('keydown', e => onKey(e, true));
    window.addEventListener('keyup', e => onKey(e, false));
    window.addEventListener('blur', () => q.push(6, 0, 0, 0));
    window.addEventListener('focus', () => q.push(6, 1, 0, 0));
    const cv = Module.canvas;
    cv.addEventListener('mousemove', e => { if (document.pointerLockElement === cv) q.push(3, e.movementX | 0, e.movementY | 0, 0); });
    cv.addEventListener('mousedown', e => { q.push(4, e.button === 2 ? 1 : e.button === 1 ? 2 : e.button, 1, 0); });
    cv.addEventListener('mouseup', e => { q.push(4, e.button === 2 ? 1 : e.button === 1 ? 2 : e.button, 0, 0); });
    cv.addEventListener('wheel', e => { q.push(5, e.deltaY < 0 ? 120 : -120, 0, 0); e.preventDefault(); }, {passive: false});
    cv.addEventListener('contextmenu', e => e.preventDefault());
    window.addEventListener('beforeunload', () => {
      // flush pending saves synchronously as far as possible
      if (PM.saveTimer) { clearTimeout(PM.saveTimer); PM.saveTimer = 0; }
    });
    Module.resizeCanvas && Module.resizeCanvas();
  },

  js_fatal__deps: ['$PM'],
  js_fatal: function (msg) { Module.onFatal && Module.onFatal(UTF8ToString(msg)); },
  js_exit: function (code) { Module.onExit && Module.onExit(code); },

  // ---------------------------------------------------------------- files
  js_fopen__deps: ['$PM'],
  js_fopen__async: true,
  js_fopen: function (pathp, mode) {
    const path = UTF8ToString(pathp);
    const key = PM.norm(path);
    const alloc = f => { let i = PM.fds.indexOf(null); if (i < 0) { i = PM.fds.length; PM.fds.push(null); } PM.fds[i] = f; return i; };
    if (key.startsWith('save/') || key === 'save') {
      let s = PM.saves.get(key);
      if (mode === 0 || mode === 2) { if (!s) return -1; }
      if (mode === 1 || (!s && mode === 3)) {
        s = {name: path, data: new Uint8Array(0)};
        PM.saves.set(key, s);
        PM.parentDirs(key);
        PM.persist(key);
      }
      return alloc({key: null, save: s, saveKey: key, size: s.data.length, writable: mode !== 0});
    }
    if (mode !== 0) {
      // writing into the game directory: keep a copy-on-write version in the save area
      const sk = 'save/gamedir/' + key;
      let s = PM.saves.get(sk);
      if (!s) {
        s = {name: path, data: new Uint8Array(0)};
        PM.saves.set(sk, s);
        PM.parentDirs(sk);
        return PM.fetchFile(key).then(d => {
          if (d && mode !== 1) s.data = d.slice();
          PM.persist(sk);
          return alloc({key: null, save: s, saveKey: sk, size: s.data.length, writable: true});
        });
      }
      if (mode === 1) { s.data = new Uint8Array(0); PM.persist(sk); }
      return alloc({key: null, save: s, saveKey: sk, size: s.data.length, writable: true});
    }
    const shadow = PM.saves.get('save/gamedir/' + key);
    if (shadow) return alloc({key: null, save: shadow, saveKey: 'save/gamedir/' + key, size: shadow.data.length, writable: false});
    if (!PM.files.has(key)) return -1;
    const hit = PM.cache.get(key);
    if (hit) { PM.cache.delete(key); PM.cache.set(key, hit); return alloc({key, data: hit, size: hit.length}); }
    return PM.fetchFile(key).then(d => d ? alloc({key, data: d, size: d.length}) : -1);
  },
  js_fsize__deps: ['$PM'],
  js_fsize: function (fd) { const f = PM.fds[fd]; return f ? (f.save ? f.save.data.length : f.size) : 0; },
  js_fread__deps: ['$PM'],
  js_fread: function (fd, off, dst, len) {
    const f = PM.fds[fd];
    if (!f) return -1;
    const data = f.save ? f.save.data : f.data;
    off >>>= 0;
    if (off >= data.length) return 0;
    const n = Math.min(len, data.length - off);
    HEAPU8.set(data.subarray(off, off + n), dst >>> 0);
    return n;
  },
  js_fwrite__deps: ['$PM'],
  js_fwrite: function (fd, off, src, len) {
    const f = PM.fds[fd];
    if (!f || !f.save || !f.writable) return -1;
    off >>>= 0;
    const s = f.save;
    const end = off + len;
    if (end > s.data.length) {
      const nd = new Uint8Array(end);
      nd.set(s.data);
      s.data = nd;
    }
    s.data.set(HEAPU8.subarray(src >>> 0, (src >>> 0) + len), off);
    PM.persist(f.saveKey);
    return len;
  },
  js_fsetsize__deps: ['$PM'],
  js_fsetsize: function (fd, size) {
    const f = PM.fds[fd];
    if (!f || !f.save || !f.writable) return;
    size >>>= 0;
    const nd = new Uint8Array(size);
    nd.set(f.save.data.subarray(0, Math.min(size, f.save.data.length)));
    f.save.data = nd;
    PM.persist(f.saveKey);
  },
  js_fclose__deps: ['$PM'],
  js_fclose: function (fd) { if (PM.fds[fd]) PM.fds[fd] = null; },
  js_fexists__deps: ['$PM'],
  js_fexists: function (pathp) {
    const key = PM.norm(UTF8ToString(pathp));
    if (key.startsWith('save')) {
      if (PM.saves.has(key)) return 1;
      if (PM.saveDirs.has(key)) return 2;
      for (const k of PM.saves.keys()) if (k.startsWith(key + '/')) return 2;
      return 0;
    }
    if (PM.saves.has('save/gamedir/' + key)) return 1;
    if (PM.files.has(key)) return 1;
    if (PM.dirs.has(key)) return 2;
    return 0;
  },
  js_fdelete__deps: ['$PM'],
  js_fdelete: function (pathp) {
    let key = PM.norm(UTF8ToString(pathp));
    if (!key.startsWith('save')) key = 'save/gamedir/' + key;
    if (!PM.saves.has(key)) return 0;
    PM.saves.delete(key);
    PM.persist(key);
    return 1;
  },
  js_mkdir__deps: ['$PM'],
  js_mkdir: function (pathp) {
    const key = PM.norm(UTF8ToString(pathp));
    if (!key.startsWith('save')) return;
    PM.parentDirs(key + '/x');
  },

  // ---------------------------------------------------------------- frames / time
  js_present__async: true,
  js_present: function () {
    const st = Module.stats || (Module.stats = {frames: 0, busyMs: 0, last: 0});
    const now = performance.now();
    st.frames++;
    if (st.last) st.busyMs += now - st.last;
    return new Promise(res => {
      let done = false;
      const fin = () => { if (!done) { done = true; st.last = performance.now(); res(); } };
      // Animation frames pace the game (vsync). When they stop coming (background tab, covered
      // or minimized window) a worker timer takes over: workers are not throttled like page
      // timers (about one per second in background tabs), so the game keeps ~60 fps and online
      // peers do not time out.
      if (!PM.ticker) {
        const src = 'setInterval(()=>postMessage(0),16)';
        PM.ticker = new Worker(URL.createObjectURL(new Blob([src], {type: 'text/javascript'})));
        PM.tickWaiters = [];
        PM.ticker.onmessage = () => { const w = PM.tickWaiters; PM.tickWaiters = []; w.forEach(f => f()); };
      }
      const t0 = performance.now();
      const wait = () => { if (document.hidden || performance.now() - t0 > 34) fin(); else PM.tickWaiters.push(wait); };
      PM.tickWaiters.push(wait);
      if (!document.hidden) requestAnimationFrame(fin);
    });
  },
  js_sleep__async: true,
  js_sleep: function (ms) {
    const st = Module.stats || (Module.stats = {frames: 0, busyMs: 0, last: 0});
    const t0 = performance.now();
    st.sleeps = (st.sleeps || 0) + 1;
    return new Promise(res => setTimeout(() => { st.sleepMs = (st.sleepMs || 0) + performance.now() - t0; res(); }, ms));
  },
  js_yield__async: true,
  js_yield: function () {
    const st = Module.stats || (Module.stats = {frames: 0, busyMs: 0, last: 0});
    const t0 = performance.now();
    st.yields = (st.yields || 0) + 1;
    return new Promise(res => setTimeout(() => { st.sleepMs = (st.sleepMs || 0) + performance.now() - t0; res(); }, 0));
  },
  js_canvas_width: function () { return Module.canvas.width; },
  js_canvas_height: function () { return Module.canvas.height; },
  js_set_canvas_size__deps: ['$PM'],
  js_set_canvas_size: function (w, h) { PM.gameW = w; PM.gameH = h; Module.gameAspect = w / h; Module.resizeCanvas && Module.resizeCanvas(); },
  js_set_cursor_visible: function (v) { Module.canvas.style.cursor = v ? 'default' : 'none'; },
  js_message_box: function (t, c) { console.warn('[MessageBox] ' + UTF8ToString(c) + ': ' + UTF8ToString(t)); Module.onMessageBox && Module.onMessageBox(UTF8ToString(t), UTF8ToString(c)); },

  // ---------------------------------------------------------------- input
  js_poll_events__deps: ['$PM'],
  js_poll_events: function (buf, max) {
    const q = PM.events;
    const n = Math.min(max, q.length >> 2);
    if (!n) return 0;
    HEAP32.set(q.slice(0, n * 4), buf >> 2);
    q.splice(0, n * 4);
    return n;
  },
  js_gamepad_count: function () {
    const pads = navigator.getGamepads ? navigator.getGamepads() : [];
    let n = 0;
    for (const p of pads) if (p && p.connected) n++;
    return n;
  },
  js_gamepad_state: function (index, axesp, btnp, povp) {
    const pads = navigator.getGamepads ? navigator.getGamepads() : [];
    let pad = null, k = 0;
    for (const p of pads) if (p && p.connected) { if (k === index) { pad = p; break; } k++; }
    if (!pad) return 0;
    const ax = i => (pad.axes[i] || 0);
    const bv = i => (pad.buttons[i] ? pad.buttons[i].value : 0);
    const bp = i => (pad.buttons[i] ? pad.buttons[i].pressed : false);
    const cl = v => Math.max(-32768, Math.min(32767, Math.round(v * 32767)));
    const a = [cl(ax(0)), cl(ax(1)), cl(bv(6) - bv(7)), cl(ax(2)), cl(ax(3)), 0];
    for (let i = 0; i < 6; i++) HEAP32[(axesp >> 2) + i] = a[i];
    const order = [0, 1, 2, 3, 4, 5, 8, 9, 10, 11]; // A B X Y LB RB Back Start LS RS
    let b = 0;
    order.forEach((src, i) => { if (bp(src)) b |= 1 << i; });
    HEAPU32[btnp >> 2] = b;
    const up = bp(12), dn = bp(13), lf = bp(14), rt = bp(15);
    let pov = -1;
    if (up && rt) pov = 4500; else if (rt && dn) pov = 13500; else if (dn && lf) pov = 22500; else if (lf && up) pov = 31500;
    else if (up) pov = 0; else if (rt) pov = 9000; else if (dn) pov = 18000; else if (lf) pov = 27000;
    HEAP32[povp >> 2] = pov;
    return 1;
  },

  // ---------------------------------------------------------------- audio (FMOD backend)
  js_audio_init__deps: ['$PM'],
  js_audio_init: function () {
    if (!PM.audio) {
      PM.audio = Module.audioContext || new AudioContext();
      PM.master = PM.audio.createGain();
      PM.master.connect(PM.audio.destination);
    }
    if (PM.audio.state === 'suspended') PM.audio.resume();
  },
  js_snd_create_pcm__deps: ['$PM'],
  js_snd_create_pcm: function (data, bytes, bits, channels, isSigned, rate) {
    if (!PM.audio) return -1;
    const frames = Math.floor(bytes / (bits / 8) / channels);
    if (frames <= 0) return -1;
    const buf = PM.audio.createBuffer(channels, frames, rate);
    for (let c = 0; c < channels; c++) {
      const out = buf.getChannelData(c);
      if (bits === 16) {
        const base = data >>> 1;
        if (data & 1) {
          const dv = new DataView(HEAPU8.buffer, data, bytes);
          for (let i = 0; i < frames; i++) { let v = dv.getInt16((i * channels + c) * 2, true); if (!isSigned) v = (v ^ 0x8000) << 16 >> 16; out[i] = v / 32768; }
        } else {
          for (let i = 0; i < frames; i++) { let v = HEAP16[base + i * channels + c]; if (!isSigned) v = (v ^ 0x8000) << 16 >> 16; out[i] = v / 32768; }
        }
      } else {
        for (let i = 0; i < frames; i++) { let v = HEAPU8[data + i * channels + c]; out[i] = isSigned ? ((v << 24 >> 24) / 128) : (v - 128) / 128; }
      }
    }
    const id = PM.sndNext++;
    PM.sndBufs.set(id, buf);
    return id;
  },
  js_snd_load_file__deps: ['$PM'],
  js_snd_load_file__async: true,
  js_snd_load_file: async function (pathp) {
    const key = PM.norm(UTF8ToString(pathp));
    if (!PM.audio) return -1;
    const data = await PM.fetchFile(key);
    if (!data) return -1;
    try {
      const buf = await PM.audio.decodeAudioData(data.slice().buffer);
      PM.cache.delete(key);
      PM.cacheBytes -= data.length;
      const id = PM.sndNext++;
      PM.sndBufs.set(id, buf);
      return id;
    } catch (e) {
      console.error('decodeAudioData failed for', key, e);
      return -1;
    }
  },
  js_snd_rate__deps: ['$PM'],
  js_snd_rate: function (id) { const b = PM.sndBufs.get(id); return b ? b.sampleRate : 44100; },
  js_snd_free__deps: ['$PM'],
  js_snd_free: function (id) { PM.sndBufs.delete(id); },

  $PMch__deps: ['$PM'],
  $PMch: {
    get(i) {
      let c = PM.chans[i];
      if (!c) {
        const ctx = PM.audio;
        c = PM.chans[i] = {src: null, gain: ctx.createGain(), pan: ctx.createStereoPanner(), buf: null, loop: false,
                           rate: 1, offset: 0, startedAt: 0, paused: false, playing: false, gen: 0};
        c.gain.connect(c.pan);
        c.pan.connect(PM.master);
      }
      return c;
    },
    position(c) {
      if (!c.buf) return 0;
      let p = c.offset;
      if (c.src && !c.paused) p += (PM.audio.currentTime - c.startedAt) * c.rate;
      const d = c.buf.duration;
      if (c.loop && d > 0) p %= d;
      return Math.min(p, d);
    },
    startSource(c) {
      const ctx = PM.audio;
      const s = ctx.createBufferSource();
      s.buffer = c.buf;
      s.loop = c.loop;
      s.playbackRate.value = c.rate;
      s.connect(c.gain);
      const gen = ++c.gen;
      s.onended = () => { if (c.gen === gen && !c.paused) { c.playing = false; c.src = null; } };
      let off = c.offset;
      if (off >= c.buf.duration) off = c.loop ? off % c.buf.duration : c.buf.duration;
      s.start(0, off);
      c.src = s;
      c.startedAt = ctx.currentTime;
      c.offset = off;
    },
    stopSource(c) {
      if (c.src) { c.gen++; try { c.src.stop(); } catch (e) {} c.src.disconnect(); c.src = null; }
    },
  },
  js_ch_play__deps: ['$PM', '$PMch'],
  js_ch_play: function (i, id, loop, paused, rate, gain, pan) {
    if (!PM.audio) return;
    const c = PMch.get(i);
    PMch.stopSource(c);
    c.buf = PM.sndBufs.get(id) || null;
    c.loop = !!loop;
    c.rate = rate;
    c.offset = 0;
    c.paused = !!paused;
    c.gain.gain.value = gain;
    c.pan.pan.value = pan;
    c.playing = !!c.buf;
    if (c.buf && !c.paused) PMch.startSource(c);
  },
  js_ch_stop__deps: ['$PM', '$PMch'],
  js_ch_stop: function (i) { const c = PM.chans[i]; if (!c) return; PMch.stopSource(c); c.playing = false; c.paused = false; },
  js_ch_is_playing__deps: ['$PM'],
  js_ch_is_playing: function (i) { const c = PM.chans[i]; return c && c.playing ? 1 : 0; },
  js_ch_set_paused__deps: ['$PM', '$PMch'],
  js_ch_set_paused: function (i, p) {
    const c = PM.chans[i];
    if (!c || !c.buf) return;
    if (p && !c.paused) { c.offset = PMch.position(c); PMch.stopSource(c); c.paused = true; }
    else if (!p && c.paused) { c.paused = false; if (c.playing) PMch.startSource(c); }
  },
  js_ch_set_gain__deps: ['$PM'],
  js_ch_set_gain: function (i, g) { const c = PM.chans[i]; if (c) c.gain.gain.value = g; },
  js_ch_set_pan__deps: ['$PM'],
  js_ch_set_pan: function (i, p) { const c = PM.chans[i]; if (c) c.pan.pan.value = p; },
  js_ch_set_rate__deps: ['$PM', '$PMch'],
  js_ch_set_rate: function (i, r) {
    const c = PM.chans[i];
    if (!c || c.rate === r) return;
    if (c.src && !c.paused) { c.offset = PMch.position(c); c.startedAt = PM.audio.currentTime; c.src.playbackRate.value = r; }
    c.rate = r;
  },
  js_ch_set_position__deps: ['$PM', '$PMch'],
  js_ch_set_position: function (i, sec) {
    const c = PM.chans[i];
    if (!c || !c.buf) return;
    c.offset = sec;
    if (c.src && !c.paused) { PMch.stopSource(c); PMch.startSource(c); c.playing = true; }
  },
  // ---------------------------------------------------------------- online (virtual LAN, see web/online.js)
  js_net_ip: function () { const n = Module.net; return n && n.ip ? n.ip | 0 : 0; },
  js_net_send: function (ptr, len) { const n = Module.net; if (n && n.ws && n.ws.readyState === 1) n.ws.send(HEAPU8.slice(ptr, ptr + len)); },
  js_net_recv: function (buf, max) {
    const n = Module.net;
    if (!n || !n.inbox.length) return 0;
    const p = n.inbox.shift();
    if (p.length > max) return 0;
    HEAPU8.set(p, buf);
    return p.length;
  },
  js_net_name__deps: ['$stringToUTF8'],
  js_net_name: function (buf, max) { const n = Module.net; stringToUTF8((n && n.name) || '', buf, max); },
  js_net_peer_left: function () { const n = Module.net; return n && n.leftIps.length ? n.leftIps.shift() | 0 : 0; },
});
