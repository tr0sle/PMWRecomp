/* user32 / gdi32 / advapi32 / shell32 / ole32 / winmm emulation: one window
 * (the canvas), a message queue fed by browser input events, and stubs for the
 * system queries the game makes at start-up. */
#include "hle.h"
#include "js.h"
#include "win32.h"
#include <emscripten.h>
#include <strings.h>

void gl_set_game_size(int w, int h);
static void nonclient(uint32_t style, int menu, int *l, int *t, int *r, int *b);

/* ------------------------------------------------------------------ windows */
typedef struct {
  uint32_t used, style, wndproc, x, y, w, h;
  char cls[64];
} Window;
#define MAX_WINDOWS 16
static Window windows[MAX_WINDOWS];
static uint32_t active_hwnd;

typedef struct { char name[64]; uint32_t wndproc; } WndClass;
static WndClass classes[8];
static int nclasses;

static Window *win_get(uint32_t h) {
  uint32_t k = h - MAIN_HWND;
  if (k >= MAX_WINDOWS || !windows[k].used) return NULL;
  return &windows[k];
}

static uint32_t call_wndproc(uint32_t hwnd, uint32_t msg, uint32_t wp, uint32_t lp) {
  Window *w = win_get(hwnd);
  if (!w || !w->wndproc) return 0;
  uint32_t a[4] = {hwnd, msg, wp, lp};
  return guest_call(w->wndproc, 4, a, 0);
}

/* ------------------------------------------------------------------ message queue */
typedef struct { uint32_t hwnd, msg, wp, lp, time; } Msg;
#define QSIZE 256
static Msg queue[QSIZE];
static int qhead, qtail;
static int quit_posted;
static uint32_t quit_code;

void post_message(uint32_t hwnd, uint32_t msg, uint32_t wp, uint32_t lp) {
  int n = (qtail + 1) % QSIZE;
  if (n == qhead) return; /* full: drop */
  queue[qtail].hwnd = hwnd;
  queue[qtail].msg = msg;
  queue[qtail].wp = wp;
  queue[qtail].lp = lp;
  queue[qtail].time = host_now_ms();
  qtail = n;
}

/* ------------------------------------------------------------------ host input */
uint8_t g_keys[256];
int32_t g_mouse_dx, g_mouse_dy, g_mouse_dz;
uint8_t g_mouse_buttons[4];
int32_t g_mouse_x, g_mouse_y;

/* browser KeyboardEvent.code values are translated to scan codes in lib.js;
 * here we map scan codes to virtual keys for WM_KEYDOWN. */
static const uint8_t scan_to_vk[128] = {
  0, 0x1B, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', 0xBD, 0xBB, 0x08, 0x09,
  'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', 0xDB, 0xDD, 0x0D, 0x11, 'A', 'S',
  'D', 'F', 'G', 'H', 'J', 'K', 'L', 0xBA, 0xDE, 0xC0, 0x10, 0xDC, 'Z', 'X', 'C', 'V',
  'B', 'N', 'M', 0xBC, 0xBE, 0xBF, 0x10, 0x6A, 0x12, 0x20, 0x14, 0x70, 0x71, 0x72, 0x73, 0x74,
  0x75, 0x76, 0x77, 0x78, 0x79, 0x90, 0x91, 0x67, 0x68, 0x69, 0x6D, 0x64, 0x65, 0x66, 0x6B, 0x61,
  0x62, 0x63, 0x60, 0x6E, 0, 0, 0, 0x7A, 0x7B, 0, 0, 0, 0, 0, 0, 0,
};

static uint32_t vk_of_scan(int sc) {
  switch (sc) {
    case 0xC8: return 0x26; case 0xD0: return 0x28; case 0xCB: return 0x25; case 0xCD: return 0x27;
    case 0x9C: return 0x0D; case 0x9D: return 0x11; case 0xB8: return 0x12; case 0xC7: return 0x24;
    case 0xCF: return 0x23; case 0xC9: return 0x21; case 0xD1: return 0x22; case 0xD2: return 0x2D;
    case 0xD3: return 0x2E; case 0xB5: return 0x6F;
  }
  return sc < 128 ? scan_to_vk[sc] : 0;
}

void dinput_key_event(int scan, int down);
void dinput_mouse_event(int type, int a, int b);

enum { EV_KEY = 1, EV_CHAR = 2, EV_MOUSEMOVE = 3, EV_MOUSEBTN = 4, EV_WHEEL = 5, EV_FOCUS = 6, EV_RESIZE = 7 };

void pump_host_events(void) {
  int32_t ev[64 * 4];
  int n = js_poll_events(ev, 64);
  for (int k = 0; k < n; k++) {
    int32_t *e = &ev[k * 4];
    uint32_t hwnd = active_hwnd ? active_hwnd : MAIN_HWND;
    switch (e[0]) {
      case EV_KEY: {
        int sc = e[1] & 0xff, down = e[2];
        g_keys[sc] = down ? 0x80 : 0;
        dinput_key_event(sc, down);
        uint32_t vk = vk_of_scan(sc);
        if (vk) {
          uint32_t lp = 1u | ((uint32_t)(sc & 0x7f) << 16) | (sc & 0x80 ? 1u << 24 : 0) | (down ? 0 : 0xC0000000u);
          post_message(hwnd, down ? 0x100 : 0x101, vk, lp);
        }
        break;
      }
      case EV_CHAR:
        post_message(hwnd, 0x102, (uint32_t)e[1], 1);
        break;
      case EV_MOUSEMOVE:
        g_mouse_dx += e[1]; g_mouse_dy += e[2];
        dinput_mouse_event(0, e[1], e[2]);
        break;
      case EV_MOUSEBTN:
        if (e[1] >= 0 && e[1] < 4) g_mouse_buttons[e[1]] = e[2] ? 0x80 : 0;
        dinput_mouse_event(1, e[1], e[2]);
        break;
      case EV_WHEEL:
        g_mouse_dz += e[1];
        dinput_mouse_event(2, e[1], 0);
        break;
      case EV_FOCUS:
        if (!e[1]) memset(g_keys, 0, sizeof g_keys);
        break;
      case EV_RESIZE:
        break;
    }
  }
}

/* ------------------------------------------------------------------ user32 */
HLE(user32, RegisterClassExA) {
  uint32_t wc = ARG(0);
  if (nclasses >= 8) RET(1, 0);
  WndClass *c = &classes[nclasses++];
  c->wndproc = LD32(wc + 8);
  snprintf(c->name, sizeof c->name, "%s", (char *)GPTR(LD32(wc + 40)));
  RET(1, 0xC000 + nclasses);
}

HLE(user32, CreateWindowExA) { /* (exstyle, class, title, style, x, y, w, h, parent, menu, inst, param) */
  const char *cls = ARG(1) < 0x10000 ? "" : ARGP(1);
  uint32_t wndproc = 0;
  for (int k = 0; k < nclasses; k++)
    if (!strcasecmp(classes[k].name, cls)) wndproc = classes[k].wndproc;
  for (int k = 0; k < MAX_WINDOWS; k++) {
    if (windows[k].used) continue;
    Window *w = &windows[k];
    memset(w, 0, sizeof *w);
    w->used = 1;
    w->style = ARG(3);
    w->wndproc = wndproc;
    w->x = ARG(4); w->y = ARG(5); w->w = ARG(6); w->h = ARG(7);
    snprintf(w->cls, sizeof w->cls, "%s", cls);
    uint32_t h = MAIN_HWND + k;
    LOG("CreateWindowExA(%s, %s, style=%08x, %dx%d) -> %x\n", cls, ARG(2) ? ARGP(2) : "", ARG(3), ARG(6), ARG(7), h);
    if (ARG(3) & 0x10000000u) { /* WS_VISIBLE */
      int l, t, r, b;
      nonclient(ARG(3), 0, &l, &t, &r, &b);
      int cw = (int)ARG(6) - l - r, ch = (int)ARG(7) - t - b;
      if (cw > 0 && ch > 0) gl_set_game_size(cw, ch);
      active_hwnd = h;
      post_message(h, 0x1C, 1, 0); /* WM_ACTIVATEAPP */
    }
    RET(12, h);
  }
  RET(12, 0);
}

HLE(user32, DestroyWindow) {
  uint32_t h = ARG(0);
  Window *w = win_get(h);
  if (!w) RET(1, 0);
  call_wndproc(h, 0x2, 0, 0); /* WM_DESTROY */
  w->used = 0;
  if (active_hwnd == h) active_hwnd = 0;
  /* drop queued messages for this window */
  for (int k = qhead; k != qtail; k = (k + 1) % QSIZE)
    if (queue[k].hwnd == h) queue[k].msg = 0;
  RET(1, 1);
}

HLE(user32, ShowWindow) {
  uint32_t h = ARG(0), cmd = ARG(1);
  Window *w = win_get(h);
  if (w && cmd && cmd != 6 && cmd != 2 && cmd != 7 && active_hwnd != h) {
    active_hwnd = h;
    post_message(h, 0x1C, 1, 0);
  }
  RET(2, w != NULL);
}

HLE(user32, SetWindowPos) { RET(7, 1); }
HLE(user32, GetActiveWindow) { RET(0, active_hwnd); }
HLE(user32, IsIconic) { RET(1, 0); }
HLE(user32, GetDesktopWindow) { RET(0, 0x00010000u); }
HLE(user32, GetDC) { RET(1, 0x00D00000u | (ARG(0) & 0xffff)); }
HLE(user32, ReleaseDC) { RET(2, 1); }

HLE(user32, ClientToScreen) { RET(2, 1); }
HLE(user32, SetRect) {
  uint32_t r = ARG(0);
  ST32(r, ARG(1)); ST32(r + 4, ARG(2)); ST32(r + 8, ARG(3)); ST32(r + 12, ARG(4));
  RET(5, 1);
}
/* non-client metrics of a classic Windows XP theme */
static void nonclient(uint32_t style, int menu, int *l, int *t, int *r, int *b) {
  *l = *t = *r = *b = 0;
  if ((style & 0xC00000u) == 0xC00000u) *t += 19;      /* WS_CAPTION */
  if (style & 0x40000u) { *l += 4; *t += 4; *r += 4; *b += 4; } /* WS_THICKFRAME */
  else if (style & 0x00C00000u) { *l += 3; *t += 3; *r += 3; *b += 3; }
  if (menu) *t += 19;
}

HLE(user32, AdjustWindowRect) { /* (rect, style, menu) */
  uint32_t r = ARG(0);
  int l, t, rr, b;
  nonclient(ARG(1), ARG(2) != 0, &l, &t, &rr, &b);
  ST32(r, LD32(r) - l); ST32(r + 4, LD32(r + 4) - t); ST32(r + 8, LD32(r + 8) + rr); ST32(r + 12, LD32(r + 12) + b);
  RET(3, 1);
}

static int cursor_count;
HLE(user32, ShowCursor) {
  cursor_count += ARG(0) ? 1 : -1;
  js_set_cursor_visible(cursor_count >= 0);
  RET(1, cursor_count);
}

HLE(user32, LoadIconA) { RET(2, 0x00E00001u); }
HLE(user32, LoadCursorA) { RET(2, 0x00E00002u); }

HLE(user32, MessageBoxA) {
  const char *text = ARG(1) ? ARGP(1) : "", *cap = ARG(2) ? ARGP(2) : "";
  fprintf(stderr, "MessageBoxA: [%s] %s\n", cap, text);
  js_message_box(text, cap);
  RET(4, 1); /* IDOK */
}

HLE(user32, DefWindowProcA) {
  uint32_t msg = ARG(1);
  if (msg == 0x10) { /* WM_CLOSE */
    uint32_t a[1] = {ARG(0)};
    (void)a;
    post_message(ARG(0), 0x2, 0, 0);
  }
  RET(4, 0);
}

HLE(user32, PostQuitMessage) {
  quit_posted = 1;
  quit_code = ARG(0);
  RETV(1);
}

HLE(user32, PeekMessageA) { /* (msg*, hwnd, min, max, remove) */
  uint32_t mp = ARG(0), remove = ARG(4) & 1;
  maybe_yield();
  pump_host_events();
  while (qhead != qtail && !queue[qhead].msg) qhead = (qhead + 1) % QSIZE;
  if (qhead != qtail) {
    Msg *m = &queue[qhead];
    ST32(mp, m->hwnd); ST32(mp + 4, m->msg); ST32(mp + 8, m->wp); ST32(mp + 12, m->lp);
    ST32(mp + 16, m->time); ST32(mp + 20, 0); ST32(mp + 24, 0);
    if (remove) qhead = (qhead + 1) % QSIZE;
    RET(5, 1);
  }
  if (quit_posted) {
    ST32(mp, 0); ST32(mp + 4, 0x12); ST32(mp + 8, quit_code); ST32(mp + 12, 0);
    if (remove) quit_posted = 0;
    RET(5, 1);
  }
  RET(5, 0);
}

HLE(user32, TranslateMessage) {
  /* characters come directly from the browser as WM_CHAR events */
  RET(1, 0);
}

HLE(user32, DispatchMessageA) {
  uint32_t mp = ARG(0);
  uint32_t hwnd = LD32(mp), msg = LD32(mp + 4), wp = LD32(mp + 8), lp = LD32(mp + 12);
  uint32_t r = hwnd ? call_wndproc(hwnd, msg, wp, lp) : 0;
  RET(1, r);
}

HLE(user32, EnumDisplaySettingsA) { /* (device, mode, devmode*) */
  uint32_t mode = ARG(1), dm = ARG(2);
  static const int modes[][2] = {{640, 480}, {800, 600}, {1024, 768}, {1280, 720}, {1280, 960}, {1280, 1024},
                                 {1600, 900}, {1600, 1200}, {1920, 1080}, {1920, 1200}};
  int nmodes = (int)(sizeof modes / sizeof modes[0]);
  int idx;
  if (mode == 0xFFFFFFFFu || mode == 0xFFFFFFFEu) idx = 0;
  else if ((int)mode < nmodes) idx = (int)mode;
  else RET(3, 0);
  memset(GPTR(dm), 0, 32);
  ST16(dm + 36, 156);                /* dmSize */
  ST32(dm + 40, 0x5C0000);           /* dmFields: BITSPERPEL|PELSWIDTH|PELSHEIGHT|DISPLAYFREQUENCY */
  ST32(dm + 104, 32);                /* dmBitsPerPel */
  ST32(dm + 108, modes[idx][0]);     /* dmPelsWidth */
  ST32(dm + 112, modes[idx][1]);     /* dmPelsHeight */
  ST32(dm + 120, 60);                /* dmDisplayFrequency */
  RET(3, 1);
}

HLE(user32, ChangeDisplaySettingsA) {
  uint32_t dm = ARG(0);
  if (dm) LOG("ChangeDisplaySettingsA(%ux%u)\n", LD32(dm + 108), LD32(dm + 112));
  RET(2, 0); /* DISP_CHANGE_SUCCESSFUL */
}

HLE(user32, SystemParametersInfoA) { /* (action, param, pv, winini) */
  uint32_t act = ARG(0), pv = ARG(2);
  if (act == 0x3a && pv) { /* SPI_GETSTICKYKEYS */
    ST32(pv, 8);
    ST32(pv + 4, 0);
  }
  RET(4, 1);
}

/* ------------------------------------------------------------------ gdi32 */
HLE(gdi32, GetStockObject) { RET(1, 0x00F00000u | ARG(0)); }
HLE(gdi32, GetDeviceCaps) { /* (hdc, index) */
  switch (ARG(1)) {
    case 12: RET(2, 32);    /* BITSPIXEL */
    case 8: RET(2, 1920);   /* HORZRES */
    case 10: RET(2, 1080);  /* VERTRES */
    case 116: RET(2, 60);   /* VREFRESH */
    case 14: RET(2, 1);     /* PLANES */
  }
  RET(2, 0);
}

HLE(gdi32, ChoosePixelFormat) { RET(2, 1); }
HLE(gdi32, SetPixelFormat) { RET(3, 1); }
HLE(gdi32, SwapBuffers) {
  gl_present();
  pump_host_events();
  note_yield();
  RET(1, 1);
}

/* ------------------------------------------------------------------ advapi32: registry */
HLE(advapi32, RegOpenKeyExA) { /* (key, subkey, opts, sam, result*) */
  LOG("RegOpenKeyExA(%x, %s)\n", ARG(0), ARG(1) ? ARGP(1) : "");
  RET(5, 2); /* ERROR_FILE_NOT_FOUND */
}
HLE(advapi32, RegQueryValueExA) { RET(6, 2); }
HLE(advapi32, RegCloseKey) { RET(1, 0); }

/* ------------------------------------------------------------------ shell32 / ole32 / winmm */
HLE(shell32, SHGetSpecialFolderPathA) { /* (hwnd, path, csidl, create) */
  uint32_t out = ARG(1), csidl = ARG(2) & 0xff;
  const char *p = SAVE_ROOT;
  char buf[260];
  if (csidl == 0x1a) p = SAVE_ROOT "\\AppData";        /* CSIDL_APPDATA */
  else if (csidl == 0x1c) p = SAVE_ROOT "\\LocalAppData";
  else if (csidl == 0x23) p = SAVE_ROOT "\\CommonAppData";
  snprintf(buf, sizeof buf, "%s", p);
  strcpy((char *)GPTR(out), buf);
  char host[512];
  path_to_host(buf, host, sizeof host);
  if (!js_fexists(host)) js_mkdir(host);
  RET(4, 1);
}

HLE(ole32, CoInitializeEx) { RET(2, 0); }
HLE(ole32, CoUninitialize) { RETV(0); }
extern uint32_t g_time_calls, g_time_caller;
HLE(winmm, timeGetTime) { g_time_calls++; g_time_caller = LD32(g_cpu.esp) | 0x40000000u; RET(0, host_now_ms()); }

void user32_init(void) {}
