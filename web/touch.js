// On-screen controls for phones and tablets: a stick on the left and buttons on the right,
// sent to the game as the keyboard keys of web/overlay/Win32.ini (KEYBOARD1).
'use strict';
(function () {
  const touchDevice = matchMedia('(pointer: coarse)').matches || navigator.maxTouchPoints > 0;
  if (!touchDevice) return;
  const SC = {up: 0xc8, down: 0xd0, left: 0xcb, right: 0xcd, enter: 0x1c, esc: 0x01, space: 0x39, shift: 0x2a, ctrl: 0x1d, c: 0x2e};
  const held = new Map(); /* scancode -> number of fingers holding it */
  const downAt = new Map();
  const MIN_HOLD = 120; /* the game polls its keys once per frame: a quick tap must last a few frames */
  function key(sc, down) {
    if (!window.Module || !Module.PM || !Module.PM.events) return;
    const n = (held.get(sc) || 0) + (down ? 1 : -1);
    held.set(sc, Math.max(0, n));
    if (down && n === 1) { downAt.set(sc, performance.now()); Module.PM.events.push(1, sc, 1, 0); }
    else if (!down && n === 0) {
      const wait = MIN_HOLD - (performance.now() - (downAt.get(sc) || 0));
      const up = () => { if (!held.get(sc)) Module.PM.events.push(1, sc, 0, 0); };
      if (wait > 0) setTimeout(up, wait); else up();
    }
  }

  const css = `
  #touch { position: fixed; inset: 0; z-index: 13; pointer-events: none; user-select: none; -webkit-user-select: none; touch-action: none; display: none; }
  #touch .stick { position: absolute; left: max(18px, env(safe-area-inset-left)); bottom: 22px; width: 150px; height: 150px; border-radius: 50%;
    background: rgba(255,255,255,.08); border: 2px solid rgba(255,255,255,.25); pointer-events: auto; touch-action: none; }
  #touch .knob { position: absolute; left: 50%; top: 50%; width: 62px; height: 62px; margin: -31px 0 0 -31px; border-radius: 50%;
    background: rgba(255,212,0,.55); border: 2px solid rgba(255,255,255,.5); }
  #touch .pad { position: absolute; right: max(14px, env(safe-area-inset-right)); bottom: 18px; display: grid;
    grid-template-columns: repeat(3, 64px); grid-template-rows: repeat(3, 64px); gap: 10px; pointer-events: none; }
  #touch .b { pointer-events: auto; touch-action: none; border-radius: 50%; display: flex; align-items: center; justify-content: center;
    font: 700 12px system-ui, sans-serif; color: #fff; background: rgba(20,24,36,.55); border: 2px solid rgba(255,255,255,.35); text-align: center; line-height: 1.1; }
  #touch .b.on { background: rgba(255,212,0,.6); color: #000; }
  #touch .go { background: rgba(40,170,70,.6); }
  #touch .top { position: absolute; top: 10px; right: max(110px, env(safe-area-inset-right)); display: flex; gap: 8px; }
  #touch .top .b { width: 58px; height: 34px; border-radius: 10px; }
  #touchToggle { position: fixed; z-index: 13; bottom: 8px; left: 50%; transform: translateX(-50%); font: 12px system-ui, sans-serif;
    padding: 4px 10px; border-radius: 8px; border: 0; background: rgba(20,24,36,.6); color: #ccc; display: none; }`;
  const style = document.createElement('style');
  style.textContent = css;
  document.head.append(style);

  const root = document.createElement('div');
  root.id = 'touch';
  root.innerHTML = `
    <div class="stick"><div class="knob"></div></div>
    <div class="top"><div class="b" data-k="esc">Pause</div><div class="b" data-k="enter">OK</div></div>
    <div class="pad">
      <div></div><div class="b" data-k="c">Big<br>Pac</div><div class="b" data-k="ctrl">Throw</div>
      <div class="b" data-k="space">Item</div><div class="b" data-k="shift">Drift</div><div class="b" data-k="down">Brake</div>
      <div></div><div class="b go" data-k="up">GO</div><div class="b" data-k="enter">OK</div>
    </div>`;
  document.body.append(root);
  const toggle = document.createElement('button');
  toggle.id = 'touchToggle';
  toggle.textContent = 'Touch controls';
  document.body.append(toggle);

  /* buttons: each finger holds its key until it lifts */
  for (const b of root.querySelectorAll('.b')) {
    const sc = SC[b.dataset.k];
    const fingers = new Set();
    b.addEventListener('pointerdown', e => {
      e.preventDefault();
      b.setPointerCapture(e.pointerId);
      fingers.add(e.pointerId);
      b.classList.add('on');
      key(sc, true);
    });
    const up = e => {
      if (!fingers.delete(e.pointerId)) return;
      if (!fingers.size) b.classList.remove('on');
      key(sc, false);
    };
    b.addEventListener('pointerup', up);
    b.addEventListener('pointercancel', up);
  }

  /* stick: 4 directions with a dead zone; diagonals press two keys */
  const stick = root.querySelector('.stick'), knob = root.querySelector('.knob');
  let stickId = null, dirs = new Set();
  function setDirs(next) {
    for (const d of dirs) if (!next.has(d)) key(SC[d], false);
    for (const d of next) if (!dirs.has(d)) key(SC[d], true);
    dirs = next;
  }
  function moveStick(e) {
    const r = stick.getBoundingClientRect();
    const cx = r.left + r.width / 2, cy = r.top + r.height / 2;
    let dx = e.clientX - cx, dy = e.clientY - cy;
    const max = r.width / 2, len = Math.hypot(dx, dy);
    if (len > max) { dx *= max / len; dy *= max / len; }
    knob.style.transform = `translate(${dx}px, ${dy}px)`;
    const dz = max * 0.3, next = new Set();
    if (dx < -dz) next.add('left'); else if (dx > dz) next.add('right');
    if (dy < -dz * 1.6) next.add('up'); else if (dy > dz * 1.6) next.add('down');
    setDirs(next);
  }
  stick.addEventListener('pointerdown', e => { e.preventDefault(); stickId = e.pointerId; stick.setPointerCapture(e.pointerId); moveStick(e); });
  stick.addEventListener('pointermove', e => { if (e.pointerId === stickId) moveStick(e); });
  const release = e => { if (e.pointerId !== stickId) return; stickId = null; knob.style.transform = ''; setDirs(new Set()); };
  stick.addEventListener('pointerup', release);
  stick.addEventListener('pointercancel', release);

  /* shown while the game runs; hidden while a gamepad is in use (the toggle brings it back) */
  let userHidden = false;
  toggle.onclick = () => { userHidden = !userHidden; update(); };
  function padInUse() {
    const pads = navigator.getGamepads ? navigator.getGamepads() : [];
    for (const p of pads) if (p && p.connected) return true;
    return false;
  }
  function update() {
    const on = !!window.gameStarted;
    const show = on && !userHidden && !padInUse() && !(Module.inputBlocked && Module.inputBlocked());
    root.style.display = show ? 'block' : 'none';
    toggle.style.display = on ? 'block' : 'none';
    toggle.textContent = show ? 'Hide touch controls' : 'Touch controls';
    if (!show && (dirs.size || held.size)) { setDirs(new Set()); for (const [sc, n] of held) if (n) { held.set(sc, 1); key(sc, false); } }
  }
  setInterval(update, 500);
  window.addEventListener('gamepadconnected', update);
  window.addEventListener('gamepaddisconnected', update);
})();
