# PMWRecomp — Pac-Man World Rally (PC) in the browser

A static recompilation of `PMR.exe` (Pac-Man World Rally for PC, 2006) to WebAssembly:
x86 → C → wasm. The browser runtime emulates Windows, OpenGL 1.1 (on WebGL2), FMOD (on
WebAudio), DirectInput (keyboard and gamepad) and Winsock. Winsock becomes a virtual LAN over
WebSocket, which brings the game's own network mode to the internet with online rooms.

**This repository does not contain the game.** It has no game files, no code generated from the
executable and no decompilation. To play, each person uses **their own copy** of the PC game.

## Playing (published site)

1. Open the site and click **Choose .zip file**.
2. Upload a .zip of the folder where the game is installed. On Windows: right-click the folder →
   *Send to* → *Compressed (zipped) folder*, or run `python tools/make_game_zip.py FOLDER`.
   - The site checks the `PMR.exe` version (SHA-256 `a017c5ec…3f4c`).
   - The files stay in your browser (IndexedDB) and are never uploaded to the server.
3. **Play solo** or **Play online**:
   - Public or private rooms (6-letter code or invite link), up to 4 players.
   - The room owner picks the mode (race, circuit, 5 battle modes), the track, CPUs and options.
   - The match starts automatically for everyone.

Requires a browser with JSPI (current Chrome or Edge).

## Controls

| Action | Xbox-style gamepad | Keyboard |
|---|---|---|
| Accelerate | A or RT | Up |
| Brake / reverse | B | Down |
| Steer | Left stick or d-pad | Left / Right |
| Hop / drift | LT or RB | Left Shift |
| Fire power-up | X | Space |
| Throw power-up | Y | Left Ctrl |
| Drop power-up | LB | X |
| Power pellet | Left stick click | C |
| Guardian | Right stick click | Z |
| Combo | D-pad up | V |
| Look back | Back/View or d-pad down | B |
| Pause | Start/Menu | Esc or P |

In menus: A / Enter confirms, B / Esc goes back. Press a button on the gamepad once the page is
open so the browser exposes it. Bindings live in `web/overlay/Win32.ini`.

## Building

Requirements: Python 3 with `pefile` and `capstone`, [emsdk](https://emscripten.org)
(Emscripten 6), Node.js 20+, bash (Git Bash on Windows) and an installed copy of the game.

```bash
export PMWR_GAME_DIR="/path/to/game"        # folder containing PMR.exe
python tools/codegen.py                      # generates gen/*.c from your PMR.exe
./build.sh                                   # builds into dist/
node server/server.js --game "$PMWR_GAME_DIR"    # http://localhost:8080
```

- With `--game`, the local server serves the game files directly, which is handy for development.
- Without `--game` (or with `--no-game`), it behaves like the public site and asks each player
  for their .zip.
- `http://localhost:8080/test2.html?n=3` opens 2 to 4 game instances side by side to test online
  play.

### Layout

| Folder | Contents |
|---|---|
| `tools/` | analyzer and code generator (x86 → C), game .zip helper |
| `runtime/` | CPU/memory, Win32, OpenGL→WebGL2, FMOD→WebAudio, DirectInput, virtual network, JS bridge |
| `web/` | page, online rooms (`online.js`), in-browser game storage (`gamestore.js`) |
| `server/` | dependency-free Node server: site files + room relay (`/ws`) |
| `deploy/` | server setup and publishing scripts |

## Publishing (your own server)

The server must run Node.js continuously and accept WebSocket connections: a Linux VPS or
virtual machine. Regular shared hosting will not work.

1. Copy `deploy/config.example.env` to `deploy/config.env` and fill in the IP, domain and SSH key.
2. The first time, run `deploy/deploy.sh --setup`. It installs Node.js, Caddy (automatic HTTPS)
   and the service.
3. After each change, run `./publish.sh "what changed"`. It builds, pushes to GitHub and updates
   the site.

`pmr.wasm` can only be built with a copy of the game, so publishing runs from the developer's
machine instead of CI.
