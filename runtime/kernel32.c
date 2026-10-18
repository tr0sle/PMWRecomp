/* kernel32.dll emulation. */
#include "hle.h"
#include "js.h"
#include "win32.h"
#include <ctype.h>
#include <time.h>
#include <emscripten.h>
#include <emscripten/html5.h>

/* ------------------------------------------------------------------ handles */
Handle g_handles[MAX_HANDLES];
static uint32_t last_error;

void set_last_error(uint32_t e) { last_error = e; }

uint32_t handle_new(int type) {
  for (int k = 1; k < MAX_HANDLES; k++)
    if (!g_handles[k].type) {
      memset(&g_handles[k], 0, sizeof g_handles[k]);
      g_handles[k].type = type;
      return HANDLE_BASE + k * 4;
    }
  fatal("out of handles");
}

Handle *handle_get(uint32_t h, int type) {
  uint32_t k = (h - HANDLE_BASE) / 4;
  if (h < HANDLE_BASE || (h & 3) || k >= MAX_HANDLES) return NULL;
  Handle *p = &g_handles[k];
  if (!p->type || (type && p->type != type)) return NULL;
  return p;
}

/* ------------------------------------------------------------------ paths */
static const char *const GAME_PREFIXES[] = {"c:/pmwr/", "c:/program files/pac-man world rally/", NULL};

/* Converts a Win32 path from the game into a host VFS path (see js.h). */
void path_to_host(const char *in, char *out, int outsz) {
  char tmp[512];
  int n = 0;
  for (const char *s = in; *s && n < (int)sizeof tmp - 1; s++) {
    char c = *s == '\\' ? '/' : *s;
    if (c == '/' && n > 0 && tmp[n - 1] == '/') continue;
    tmp[n++] = c;
  }
  tmp[n] = 0;
  char low[512];
  for (int k = 0; k <= n; k++) low[k] = (char)tolower((unsigned char)tmp[k]);
  const char *p = tmp;
  for (int k = 0; GAME_PREFIXES[k]; k++) {
    size_t l = strlen(GAME_PREFIXES[k]);
    if (!strncmp(low, GAME_PREFIXES[k], l)) { p = tmp + l; goto done; }
  }
  if (!strncmp(low, SAVE_ROOT_LOW, strlen(SAVE_ROOT_LOW))) {
    snprintf(out, outsz, "save/%s", tmp + strlen(SAVE_ROOT_LOW));
    return;
  }
  if (low[0] && low[1] == ':') { /* other absolute paths: keep inside the save area */
    snprintf(out, outsz, "save/%s", tmp + (tmp[2] == '/' ? 3 : 2));
    return;
  }
done:
  while (p[0] == '.' && p[1] == '/') p += 2;
  snprintf(out, outsz, "%s", p);
  /* strip trailing slash */
  n = (int)strlen(out);
  if (n > 0 && out[n - 1] == '/') out[n - 1] = 0;
}

/* ------------------------------------------------------------------ strings / locale */
static uint32_t cmdline_a, env_a, env_w;

HLE(kernel32, GetCommandLineA) { RET(0, cmdline_a); }
HLE(kernel32, GetEnvironmentStrings) { RET(0, env_a); }
HLE(kernel32, GetEnvironmentStringsW) { RET(0, env_w); }
HLE(kernel32, FreeEnvironmentStringsA) { RET(1, 1); }
HLE(kernel32, FreeEnvironmentStringsW) { RET(1, 1); }
HLE(kernel32, SetEnvironmentVariableA) { RET(2, 1); }

HLE(kernel32, GetACP) { RET(0, 1252); }
HLE(kernel32, GetOEMCP) { RET(0, 437); }

HLE(kernel32, GetCPInfo) {
  uint32_t p = ARG(1);
  memset(GPTR(p), 0, 20);
  ST32(p, 1);       /* MaxCharSize */
  ST8(p + 4, '?');  /* DefaultChar */
  RET(2, 1);
}

static uint16_t ctype1(uint32_t c) {
  /* C1_UPPER 1, C1_LOWER 2, C1_DIGIT 4, C1_SPACE 8, C1_PUNCT 0x10, C1_CNTRL 0x20, C1_BLANK 0x40, C1_XDIGIT 0x80, C1_ALPHA 0x100 */
  uint16_t r = 0;
  if (c < 0x80) {
    if (isupper(c)) r |= 0x101;
    if (islower(c)) r |= 0x102;
    if (isdigit(c)) r |= 4;
    if (isspace(c)) r |= 8;
    if (ispunct(c)) r |= 0x10;
    if (iscntrl(c)) r |= 0x20;
    if (c == ' ' || c == '\t') r |= 0x40;
    if (isxdigit(c)) r |= 0x80;
  } else if (c >= 0xC0 && c <= 0xFF && c != 0xD7 && c != 0xF7) {
    r |= c < 0xDF ? 0x101 : 0x102;
  } else if (c == 0xA0) r |= 0x48;
  else if (c >= 0xA1 && c <= 0xBF) r |= 0x10;
  else if (c >= 0x80 && c < 0xA0) r |= 0x20;
  return r;
}

HLE(kernel32, GetStringTypeA) { /* (lcid, type, src, cch, out) */
  uint32_t type = ARG(1), src = ARG(2), out = ARG(4);
  int n = (int)ARG(3);
  if (n < 0) n = (int)strlen((char *)GPTR(src)) + 1;
  for (int k = 0; k < n; k++) ST16(out + 2 * k, type == 1 ? ctype1(LD8(src + k)) : 0);
  RET(5, 1);
}

HLE(kernel32, GetStringTypeW) { /* (type, src, cch, out) */
  uint32_t type = ARG(0), src = ARG(1), out = ARG(3);
  int n = (int)ARG(2);
  if (n < 0) { n = 0; while (LD16(src + 2 * n)) n++; n++; }
  for (int k = 0; k < n; k++) {
    uint32_t c = LD16(src + 2 * k);
    ST16(out + 2 * k, type == 1 ? (c < 0x100 ? ctype1(c) : 0x100) : 0);
  }
  RET(4, 1);
}

static uint32_t map_case(uint32_t c, uint32_t flags) {
  if (flags & 0x100) { if ((c >= 'A' && c <= 'Z') || (c >= 0xC0 && c <= 0xDE && c != 0xD7)) c += 32; }
  else if (flags & 0x200) { if ((c >= 'a' && c <= 'z') || (c >= 0xE0 && c <= 0xFE && c != 0xF7)) c -= 32; }
  return c;
}

HLE(kernel32, LCMapStringA) { /* (lcid, flags, src, cch, dst, cchdst) */
  uint32_t flags = ARG(1), src = ARG(2), dst = ARG(4);
  int n = (int)ARG(3), nd = (int)ARG(5);
  if (n < 0) n = (int)strlen((char *)GPTR(src)) + 1;
  if (nd == 0) RET(6, n);
  if (nd < n) { set_last_error(122); RET(6, 0); }
  for (int k = 0; k < n; k++) ST8(dst + k, map_case(LD8(src + k), flags));
  RET(6, n);
}

HLE(kernel32, LCMapStringW) {
  uint32_t flags = ARG(1), src = ARG(2), dst = ARG(4);
  int n = (int)ARG(3), nd = (int)ARG(5);
  if (n < 0) { n = 0; while (LD16(src + 2 * n)) n++; n++; }
  if (nd == 0) RET(6, n);
  if (nd < n) { set_last_error(122); RET(6, 0); }
  for (int k = 0; k < n; k++) ST16(dst + 2 * k, map_case(LD16(src + 2 * k), flags));
  RET(6, n);
}

static int cmp_strings(uint32_t a, int na, uint32_t b, int nb, int wide, uint32_t flags) {
  int k = 0;
  for (;; k++) {
    int ea = k >= na, eb = k >= nb;
    if (ea || eb) return ea && eb ? 2 : ea ? 1 : 3;
    uint32_t ca = wide ? LD16(a + 2 * k) : LD8(a + k), cb = wide ? LD16(b + 2 * k) : LD8(b + k);
    if (flags & 1) { ca = map_case(ca, 0x100); cb = map_case(cb, 0x100); }
    if (ca != cb) return ca < cb ? 1 : 3;
  }
}

HLE(kernel32, CompareStringA) { /* (lcid, flags, s1, n1, s2, n2) */
  uint32_t a = ARG(2), b = ARG(4);
  int na = (int)ARG(3), nb = (int)ARG(5);
  if (na < 0) na = (int)strlen((char *)GPTR(a));
  if (nb < 0) nb = (int)strlen((char *)GPTR(b));
  RET(6, cmp_strings(a, na, b, nb, 0, ARG(1)));
}

HLE(kernel32, CompareStringW) {
  uint32_t a = ARG(2), b = ARG(4);
  int na = (int)ARG(3), nb = (int)ARG(5);
  if (na < 0) { na = 0; while (LD16(a + 2 * na)) na++; }
  if (nb < 0) { nb = 0; while (LD16(b + 2 * nb)) nb++; }
  RET(6, cmp_strings(a, na, b, nb, 1, ARG(1)));
}

HLE(kernel32, GetLocaleInfoA) { /* (lcid, type, buf, cch) */
  uint32_t type = ARG(1) & 0xffff, buf = ARG(2);
  int cch = (int)ARG(3);
  const char *v = "";
  switch (type) {
    case 0x1004: v = "1252"; break;  /* LOCALE_IDEFAULTANSICODEPAGE */
    case 0x000B: v = "437"; break;   /* LOCALE_IDEFAULTCODEPAGE */
    case 0x0001: v = "0409"; break;  /* LOCALE_ILANGUAGE */
    case 0x000E: v = "."; break;     /* LOCALE_SDECIMAL */
    case 0x000F: v = ","; break;     /* LOCALE_STHOUSAND */
    case 0x0010: v = "3;0"; break;   /* LOCALE_SGROUPING */
    case 0x0014: v = "$"; break;
    case 0x0059: v = "en"; break;
    case 0x005A: v = "US"; break;
    default: v = ""; break;
  }
  int n = (int)strlen(v) + 1;
  if (cch == 0) RET(4, n);
  if (cch < n) { set_last_error(122); RET(4, 0); }
  memcpy(GPTR(buf), v, n);
  RET(4, n);
}

HLE(kernel32, MultiByteToWideChar) { /* (cp, flags, src, cb, dst, cch) */
  uint32_t src = ARG(2), dst = ARG(4);
  int n = (int)ARG(3), nd = (int)ARG(5);
  if (n < 0) n = (int)strlen((char *)GPTR(src)) + 1;
  if (nd == 0) RET(6, n);
  if (nd < n) { set_last_error(122); RET(6, 0); }
  for (int k = 0; k < n; k++) ST16(dst + 2 * k, LD8(src + k));
  RET(6, n);
}

HLE(kernel32, WideCharToMultiByte) { /* (cp, flags, src, cch, dst, cb, defchar, useddef) */
  uint32_t src = ARG(2), dst = ARG(4), used = ARG(7);
  int n = (int)ARG(3), nd = (int)ARG(5);
  if (n < 0) { n = 0; while (LD16(src + 2 * n)) n++; n++; }
  if (used) ST32(used, 0);
  if (nd == 0) RET(8, n);
  if (nd < n) { set_last_error(122); RET(8, 0); }
  for (int k = 0; k < n; k++) {
    uint32_t c = LD16(src + 2 * k);
    if (c > 0xff) { c = '?'; if (used) ST32(used, 1); }
    ST8(dst + k, c);
  }
  RET(8, n);
}

/* ------------------------------------------------------------------ misc process / system */
HLE(kernel32, GetLastError) { RET(0, last_error); }
HLE(kernel32, SetLastError) { last_error = ARG(0); RETV(1); }
HLE(kernel32, GetCurrentProcess) { RET(0, 0xFFFFFFFFu); }
HLE(kernel32, GetCurrentProcessId) { RET(0, 1); }
HLE(kernel32, GetCurrentThreadId) { RET(0, cur_thread_id()); }
HLE(kernel32, ExitProcess) { process_exit(ARG(0)); }
HLE(kernel32, TerminateProcess) { process_exit(ARG(1)); }
HLE(kernel32, UnhandledExceptionFilter) { RET(1, 1); }

static uint32_t unhandled_filter;
HLE(kernel32, SetUnhandledExceptionFilter) {
  uint32_t old = unhandled_filter;
  unhandled_filter = ARG(0);
  RET(1, old);
}

HLE(kernel32, RaiseException) {
  fatal("RaiseException code=%08x flags=%x nargs=%u (C++ exception or runtime error in the game)", ARG(0), ARG(1), ARG(2));
}

HLE(kernel32, RtlUnwind) { /* (TargetFrame, TargetIp, ExceptionRecord, ReturnValue) */
  if (ARG(0)) ST32(FS_BASE, ARG(0));
  RETV(4);
}

HLE(kernel32, GetVersionExA) {
  uint32_t p = ARG(0);
  uint32_t sz = LD32(p);
  memset(GPTR(p + 4), 0, sz > 4 ? sz - 4 : 0);
  ST32(p + 4, 5);       /* Windows XP */
  ST32(p + 8, 1);
  ST32(p + 12, 2600);
  ST32(p + 16, 2);      /* VER_PLATFORM_WIN32_NT */
  strcpy((char *)GPTR(p + 20), "Service Pack 2");
  RET(1, 1);
}

HLE(kernel32, GetStartupInfoA) {
  uint32_t p = ARG(0);
  memset(GPTR(p), 0, 68);
  ST32(p, 68);
  RETV(1);
}

HLE(kernel32, GetSystemInfo) {
  uint32_t p = ARG(0);
  memset(GPTR(p), 0, 36);
  ST32(p + 4, 4096);
  ST32(p + 8, 0x10000);
  ST32(p + 12, 0x7FFEFFFF);
  ST32(p + 16, 1);
  ST32(p + 20, 1);
  ST32(p + 24, 586);
  ST32(p + 28, 0x10000);
  ST16(p + 32, 6);
  ST16(p + 34, 0x0801);
  RETV(1);
}

HLE(kernel32, GetModuleFileNameA) { /* (hmod, buf, size) */
  const char *nm = "C:\\PMWR\\PMR.exe";
  uint32_t buf = ARG(1), n = ARG(2);
  uint32_t l = (uint32_t)strlen(nm);
  if (n == 0) RET(3, 0);
  if (l >= n) l = n - 1;
  memcpy(GPTR(buf), nm, l);
  ST8(buf + l, 0);
  RET(3, l);
}

/* ------------------------------------------------------------------ modules */
typedef struct { const char *name; uint32_t handle; } Module;
static const Module modules[] = {
  {"kernel32", 0x7C800000u}, {"user32", 0x7E410000u}, {"gdi32", 0x77F10000u}, {"opengl32", 0x5ED00000u},
  {"glu32", 0x68B20000u}, {"fmod", 0x10000000u}, {"dinput8", 0x4CF20000u}, {"advapi32", 0x77DD0000u},
  {"ws2_32", 0x71AB0000u}, {"winmm", 0x76B40000u}, {"ole32", 0x774E0000u}, {"shell32", 0x7C9C0000u},
  {"d3d8", 0x6D9A0000u}, /* only probed by WinMain as a DirectX presence check */
  {NULL, 0}};

static const Module *module_by_name(const char *nm) {
  char b[64];
  int n = 0;
  const char *s = strrchr(nm, '\\');
  s = s ? s + 1 : nm;
  for (; *s && *s != '.' && n < 63; s++) b[n++] = (char)tolower((unsigned char)*s);
  b[n] = 0;
  for (int k = 0; modules[k].name; k++)
    if (!strcmp(modules[k].name, b)) return &modules[k];
  return NULL;
}

static const char *module_by_handle(uint32_t h) {
  for (int k = 0; modules[k].name; k++)
    if (modules[k].handle == h) return modules[k].name;
  return NULL;
}

HLE(kernel32, GetModuleHandleA) {
  uint32_t nm = ARG(0);
  if (!nm) RET(1, IMAGE_BASE);
  const Module *m = module_by_name(ARGP(0));
  if (!m) { set_last_error(126); RET(1, 0); }
  RET(1, m->handle);
}

HLE(kernel32, LoadLibraryA) {
  const Module *m = module_by_name(ARGP(0));
  LOG("LoadLibraryA(%s) -> %s\n", ARGP(0), m ? "ok" : "not found");
  if (!m) { set_last_error(126); RET(1, 0); }
  RET(1, m->handle);
}

HLE(kernel32, FreeLibrary) { RET(1, 1); }

HLE(kernel32, GetProcAddress) {
  uint32_t h = ARG(0), nm = ARG(1);
  const char *mod = module_by_handle(h);
  if (nm < 0x10000) { LOG("GetProcAddress(%s, #%u): ordinals unsupported\n", mod ? mod : "?", nm); RET(2, 0); }
  const char *name = ARGP(1);
  uint32_t r = mod ? hle_lookup(mod, name) : 0;
  LOG("GetProcAddress(%s, %s) -> %08x\n", mod ? mod : "?", name, r);
  if (!r) set_last_error(127);
  RET(2, r);
}

/* ------------------------------------------------------------------ time */
uint32_t g_time_calls; /* polled-clock reads, for diagnosing busy-wait loops */
uint32_t g_time_caller;
EMSCRIPTEN_KEEPALIVE uint32_t time_calls(void) { return g_time_calls; }
EMSCRIPTEN_KEEPALIVE uint32_t time_caller(void) { return g_time_caller; }
HLE(kernel32, GetTickCount) { g_time_calls++; g_time_caller = LD32(g_cpu.esp) | 0x80000000u; RET(0, host_now_ms()); }
HLE(kernel32, QueryPerformanceFrequency) { ST64(ARG(0), 1000000ull); RET(1, 1); }
HLE(kernel32, QueryPerformanceCounter) { g_time_calls++; g_time_caller = LD32(g_cpu.esp); ST64(ARG(0), (uint64_t)host_now_us()); RET(1, 1); }

static uint64_t filetime_now(void) {
  double ms = emscripten_date_now();
  return (uint64_t)((ms + 11644473600000.0) * 10000.0);
}

HLE(kernel32, GetSystemTimeAsFileTime) { ST64(ARG(0), filetime_now()); RETV(1); }

HLE(kernel32, GetSystemTime) {
  uint32_t p = ARG(0);
  double ms = emscripten_date_now();
  time_t t = (time_t)(ms / 1000.0);
  struct tm tmv;
  gmtime_r(&t, &tmv);
  ST16(p + 0, tmv.tm_year + 1900);
  ST16(p + 2, tmv.tm_mon + 1);
  ST16(p + 4, tmv.tm_wday);
  ST16(p + 6, tmv.tm_mday);
  ST16(p + 8, tmv.tm_hour);
  ST16(p + 10, tmv.tm_min);
  ST16(p + 12, tmv.tm_sec);
  ST16(p + 14, (uint32_t)fmod(ms, 1000.0));
  RETV(1);
}

HLE(kernel32, GetTimeZoneInformation) {
  memset(GPTR(ARG(0)), 0, 172);
  RET(1, 0);
}

static double last_yield;
void maybe_yield(void) {
  double now = emscripten_get_now();
  if (now - last_yield > 30.0) {
    js_yield();
    last_yield = emscripten_get_now();
  }
}
void note_yield(void) { last_yield = emscripten_get_now(); }

HLE(kernel32, Sleep) {
  uint32_t ms = ARG(0);
  if (ms == 0) maybe_yield();
  else { js_sleep(ms > 1000 ? 1000 : (int)ms); note_yield(); }
  RETV(1);
}

/* ------------------------------------------------------------------ memory */
typedef struct { uint32_t base, size; } VRegion;
#define MAX_VREGIONS 4096
static VRegion vregions[MAX_VREGIONS];
static int nvregions;

static VRegion *vregion_find(uint32_t a) {
  for (int k = 0; k < nvregions; k++)
    if (a >= vregions[k].base && a < vregions[k].base + vregions[k].size) return &vregions[k];
  return NULL;
}

HLE(kernel32, VirtualAlloc) { /* (addr, size, type, protect) */
  uint32_t addr = ARG(0), size = ARG(1), type = ARG(2);
  if (addr) {
    VRegion *r = vregion_find(addr);
    if (r && addr + size <= r->base + r->size) RET(4, addr); /* commit inside a reservation */
    if (!(type & 0x2000)) { LOG("VirtualAlloc: commit at unreserved %08x\n", addr); }
    set_last_error(487);
    RET(4, 0);
  }
  size = (size + 0xfff) & ~0xfffu;
  uint32_t p = gmemalign(0x10000, size);
  if (!p) { set_last_error(8); RET(4, 0); }
  memset(GPTR(p), 0, size);
  if (nvregions >= MAX_VREGIONS) fatal("too many VirtualAlloc regions");
  vregions[nvregions].base = p;
  vregions[nvregions].size = size;
  nvregions++;
  RET(4, p);
}

HLE(kernel32, VirtualFree) { /* (addr, size, type) */
  uint32_t addr = ARG(0), size = ARG(1), type = ARG(2);
  VRegion *r = vregion_find(addr);
  if (!r) RET(3, 0);
  if (type & 0x8000) { /* MEM_RELEASE */
    gfree(r->base);
    *r = vregions[--nvregions];
  } else if (type & 0x4000) { /* MEM_DECOMMIT */
    if (addr + size > r->base + r->size) size = r->base + r->size - addr;
    memset(GPTR(addr), 0, size);
  }
  RET(3, 1);
}

HLE(kernel32, VirtualQuery) { /* (addr, mbi, len) */
  uint32_t a = ARG(0), p = ARG(1);
  memset(GPTR(p), 0, 28);
  uint32_t page = a & ~0xfffu;
  ST32(p, page);
  VRegion *r = vregion_find(a);
  if (r) {
    ST32(p + 4, r->base); ST32(p + 8, 4); ST32(p + 12, r->base + r->size - page);
    ST32(p + 16, 0x1000); ST32(p + 20, 4); ST32(p + 24, 0x20000);
  } else if (a >= IMAGE_BASE && a < IMAGE_END) {
    ST32(p + 4, IMAGE_BASE); ST32(p + 8, 0x80); ST32(p + 12, 0x1000);
    ST32(p + 16, 0x1000); ST32(p + 20, a < TEXT_HI ? 0x20 : 4); ST32(p + 24, 0x1000000);
  } else if (a >= g_cur_thread->stack_lo && a < g_cur_thread->stack_hi) {
    ST32(p + 4, g_cur_thread->stack_lo); ST32(p + 8, 4); ST32(p + 12, g_cur_thread->stack_hi - page);
    ST32(p + 16, 0x1000); ST32(p + 20, 4); ST32(p + 24, 0x20000);
  } else {
    ST32(p + 4, page); ST32(p + 8, 4); ST32(p + 12, 0x1000);
    ST32(p + 16, 0x1000); ST32(p + 20, 4); ST32(p + 24, 0x20000);
  }
  RET(3, 28);
}

HLE(kernel32, VirtualProtect) { if (ARG(3)) ST32(ARG(3), 4); RET(4, 1); }

static int ptr_ok(uint32_t p, uint32_t n) { return p >= 0x10000 && p < 0x20000000u && p + n <= 0x20000000u && p + n >= p; }
HLE(kernel32, IsBadReadPtr) { RET(2, ARG(1) && !ptr_ok(ARG(0), ARG(1))); }
HLE(kernel32, IsBadWritePtr) { RET(2, ARG(1) && !ptr_ok(ARG(0), ARG(1))); }
HLE(kernel32, IsBadCodePtr) { RET(1, !ptr_ok(ARG(0), 1)); }

HLE(kernel32, HeapCreate) { uint32_t h = handle_new(H_HEAP); RET(3, h); }
HLE(kernel32, HeapDestroy) { RET(1, 1); }

HLE(kernel32, HeapAlloc) { /* (heap, flags, size) */
  uint32_t flags = ARG(1), size = ARG(2);
  uint32_t p = (flags & 8) ? gcalloc(size) : gmalloc(size);
  RET(3, p);
}

HLE(kernel32, HeapFree) { gfree(ARG(2)); RET(3, 1); }

HLE(kernel32, HeapReAlloc) { /* (heap, flags, ptr, size) */
  uint32_t flags = ARG(1), p = ARG(2), size = ARG(3);
  uint32_t old = gusable(p);
  if (flags & 0x10) { /* HEAP_REALLOC_IN_PLACE_ONLY */
    if (size > old) RET(4, 0);
    RET(4, p);
  }
  uint32_t r = grealloc(p, size);
  if (r && (flags & 8) && size > old) memset(GPTR(r + old), 0, size - old);
  RET(4, r);
}

HLE(kernel32, HeapSize) { RET(3, gusable(ARG(2))); }

/* ------------------------------------------------------------------ sync / threads / TLS */
HLE(kernel32, InitializeCriticalSection) { memset(GPTR(ARG(0)), 0, 24); RETV(1); }
HLE(kernel32, DeleteCriticalSection) { RETV(1); }
HLE(kernel32, EnterCriticalSection) { RETV(1); }
HLE(kernel32, LeaveCriticalSection) { RETV(1); }

HLE(kernel32, InterlockedExchange) {
  uint32_t p = ARG(0), v = ARG(1);
  uint32_t old = LD32(p);
  ST32(p, v);
  RET(2, old);
}

HLE(kernel32, CreateMutexA) { RET(3, handle_new(H_MUTEX)); }
HLE(kernel32, ReleaseMutex) { RET(1, 1); }

HLE(kernel32, WaitForSingleObject) {
  Handle *h = handle_get(ARG(0), 0);
  if (!h) { set_last_error(6); RET(2, 0xFFFFFFFFu); }
  if (h->type == H_THREAD && !h->thread->done) {
    thread_run(h->thread);
  }
  RET(2, 0);
}

HLE(kernel32, CreateThread) { /* (sa, stack, start, param, flags, tid*) */
  uint32_t stack = ARG(1), start = ARG(2), param = ARG(3), flags = ARG(4), tidp = ARG(5);
  Thread *t = thread_new(start, param, stack ? stack : 0x100000);
  uint32_t h = handle_new(H_THREAD);
  handle_get(h, H_THREAD)->thread = t;
  if (tidp) ST32(tidp, t->id);
  /* suspended threads run at ResumeThread; others run now, to completion */
  if (!(flags & 4)) thread_run(t);
  RET(6, h);
}

HLE(kernel32, ResumeThread) {
  Handle *h = handle_get(ARG(0), H_THREAD);
  if (!h) { set_last_error(6); RET(1, 0xFFFFFFFFu); }
  thread_run(h->thread); /* runs on its own guest stack; no-op if already started */
  RET(1, 1);
}

HLE(kernel32, ExitThread) { thread_exit(ARG(0)); }

static uint8_t tls_used[TLS_SLOTS];
HLE(kernel32, TlsAlloc) {
  for (int k = 0; k < TLS_SLOTS; k++)
    if (!tls_used[k]) {
      tls_used[k] = 1;
      for (int t = 0; t < MAX_THREADS; t++) g_threads[t].tls[k] = 0;
      RET(0, k);
    }
  RET(0, 0xFFFFFFFFu);
}
HLE(kernel32, TlsFree) { uint32_t k = ARG(0); if (k < TLS_SLOTS) tls_used[k] = 0; RET(1, 1); }
HLE(kernel32, TlsGetValue) {
  uint32_t k = ARG(0);
  if (k >= TLS_SLOTS) { set_last_error(87); RET(1, 0); }
  last_error = 0;
  RET(1, g_cur_thread->tls[k]);
}
HLE(kernel32, TlsSetValue) {
  uint32_t k = ARG(0);
  if (k >= TLS_SLOTS) { set_last_error(87); RET(2, 0); }
  g_cur_thread->tls[k] = ARG(1);
  RET(2, 1);
}

HLE(kernel32, CloseHandle) {
  uint32_t hv = ARG(0);
  Handle *h = handle_get(hv, 0);
  if (!h) RET(1, 1);
  if (h->type == H_FILE) js_fclose(h->fd);
  if (h->type == H_STD) RET(1, 1);
  h->type = H_FREE;
  RET(1, 1);
}

/* ------------------------------------------------------------------ files */
static uint32_t std_handles[3];

HLE(kernel32, GetStdHandle) {
  int32_t n = (int32_t)ARG(0);
  if (n >= -12 && n <= -10) RET(1, std_handles[-10 - n]);
  RET(1, 0xFFFFFFFFu);
}
HLE(kernel32, SetStdHandle) { RET(2, 1); }

HLE(kernel32, GetFileType) {
  Handle *h = handle_get(ARG(0), 0);
  if (!h) RET(1, 0);
  RET(1, h->type == H_STD ? 2 : 1);
}

HLE(kernel32, CreateFileA) { /* (name, access, share, sa, disposition, flags, template) */
  char path[512];
  path_to_host(ARGP(0), path, sizeof path);
  uint32_t access = ARG(1), disp = ARG(4);
  int exists = js_fexists(path);
  int mode;
  if (exists == 2) { set_last_error(5); LOG("CreateFileA(%s): is a directory\n", path); RET(7, 0xFFFFFFFFu); }
  switch (disp) {
    case 1: if (exists) { set_last_error(80); RET(7, 0xFFFFFFFFu); } mode = 1; break; /* CREATE_NEW */
    case 2: mode = 1; break;                                                         /* CREATE_ALWAYS */
    case 3: if (!exists) { set_last_error(2); LOG("CreateFileA(%s): not found\n", path); RET(7, 0xFFFFFFFFu); }
            mode = (access & 0x40000000u) ? 2 : 0; break;                            /* OPEN_EXISTING */
    case 4: mode = 3; break;                                                         /* OPEN_ALWAYS */
    case 5: if (!exists) { set_last_error(2); RET(7, 0xFFFFFFFFu); } mode = 1; break; /* TRUNCATE_EXISTING */
    default: set_last_error(87); RET(7, 0xFFFFFFFFu);
  }
  int fd = js_fopen(path, mode);
  note_yield();
  if (fd < 0) { set_last_error(mode ? 5 : 2); LOG("CreateFileA(%s) failed\n", path); RET(7, 0xFFFFFFFFu); }
  uint32_t h = handle_new(H_FILE);
  Handle *p = handle_get(h, H_FILE);
  p->fd = fd;
  p->pos = 0;
  LOGV("CreateFileA(%s, disp=%u) -> %x size=%d\n", path, disp, h, js_fsize(fd));
  set_last_error((disp == 2 || disp == 4) && exists ? 183 : 0);
  RET(7, h);
}

HLE(kernel32, ReadFile) { /* (h, buf, n, read*, ovl) */
  Handle *h = handle_get(ARG(0), H_FILE);
  uint32_t buf = ARG(1), n = ARG(2), rd = ARG(3);
  if (!h) { if (rd) ST32(rd, 0); set_last_error(6); RET(5, 0); }
  int got = js_fread(h->fd, h->pos, GPTR(buf), (int)n);
  if (got < 0) got = 0;
  h->pos += (uint32_t)got;
  if (rd) ST32(rd, (uint32_t)got);
  RET(5, 1);
}

HLE(kernel32, WriteFile) {
  uint32_t hv = ARG(0), buf = ARG(1), n = ARG(2), wr = ARG(3);
  Handle *h = handle_get(hv, 0);
  if (h && h->type == H_STD) {
    fprintf(stderr, "%.*s", (int)n, (char *)GPTR(buf));
    if (wr) ST32(wr, n);
    RET(5, 1);
  }
  if (!h || h->type != H_FILE) { set_last_error(6); RET(5, 0); }
  int put = js_fwrite(h->fd, h->pos, GPTR(buf), (int)n);
  if (put < 0) { set_last_error(5); RET(5, 0); }
  h->pos += (uint32_t)put;
  if (wr) ST32(wr, (uint32_t)put);
  RET(5, 1);
}

HLE(kernel32, SetFilePointer) { /* (h, lo, hi*, method) */
  Handle *h = handle_get(ARG(0), H_FILE);
  int32_t lo = (int32_t)ARG(1);
  uint32_t hip = ARG(2), method = ARG(3);
  if (!h) { set_last_error(6); RET(4, 0xFFFFFFFFu); }
  int64_t off = lo;
  if (hip) off = (int64_t)(((uint64_t)LD32(hip) << 32) | (uint32_t)lo);
  int64_t base = method == 0 ? 0 : method == 1 ? h->pos : js_fsize(h->fd);
  int64_t np = base + off;
  if (np < 0) { set_last_error(131); RET(4, 0xFFFFFFFFu); }
  h->pos = (uint32_t)np;
  if (hip) ST32(hip, 0);
  set_last_error(0);
  RET(4, h->pos);
}

HLE(kernel32, SetEndOfFile) {
  Handle *h = handle_get(ARG(0), H_FILE);
  if (!h) RET(1, 0);
  js_fsetsize(h->fd, h->pos);
  RET(1, 1);
}

HLE(kernel32, FlushFileBuffers) { RET(1, 1); }

HLE(kernel32, GetFileAttributesA) {
  char path[512];
  path_to_host(ARGP(0), path, sizeof path);
  int e = js_fexists(path);
  LOGV("GetFileAttributesA(%s) -> %d\n", path, e);
  if (!e) { set_last_error(2); RET(1, 0xFFFFFFFFu); }
  RET(1, e == 2 ? 0x10 : 0x80);
}

HLE(kernel32, CreateDirectoryA) {
  char path[512];
  path_to_host(ARGP(0), path, sizeof path);
  if (js_fexists(path)) { set_last_error(183); RET(2, 0); }
  js_mkdir(path);
  RET(2, 1);
}

HLE(kernel32, DeleteFileA) {
  char path[512];
  path_to_host(ARGP(0), path, sizeof path);
  if (!js_fdelete(path)) { set_last_error(2); RET(1, 0); }
  RET(1, 1);
}

HLE(kernel32, GetDiskFreeSpaceExA) {
  if (ARG(1)) ST64(ARG(1), 8ull << 30);
  if (ARG(2)) ST64(ARG(2), 16ull << 30);
  if (ARG(3)) ST64(ARG(3), 8ull << 30);
  RET(4, 1);
}

HLE(kernel32, LockResource) { RET(1, ARG(0)); }

/* ------------------------------------------------------------------ init */
static uint32_t put_wide(const char *s) {
  uint32_t n = (uint32_t)strlen(s) + 1, p = sys_alloc(n * 2);
  for (uint32_t k = 0; k < n; k++) ST16(p + 2 * k, (uint8_t)s[k]);
  return p;
}

void kernel32_init(void) {
  {
    extern char g_extra_args[];
    char cl[512];
    snprintf(cl, sizeof cl, "%s%s", GAME_CMDLINE, g_extra_args);
    cmdline_a = sys_strdup(cl);
  }
  static const char env[] = "PATH=C:\\WINDOWS\\system32\0SystemRoot=C:\\WINDOWS\0";
  env_a = sys_alloc(sizeof env + 1);
  memcpy(GPTR(env_a), env, sizeof env);
  env_w = sys_alloc((sizeof env + 1) * 2);
  for (uint32_t k = 0; k < sizeof env; k++) ST16(env_w + 2 * k, (uint8_t)env[k]);
  (void)put_wide;
  for (int k = 0; k < 3; k++) {
    std_handles[k] = handle_new(H_STD);
    handle_get(std_handles[k], H_STD)->fd = k;
  }
  ST32(PEB_ADDR + 0x18, handle_new(H_HEAP));
}
