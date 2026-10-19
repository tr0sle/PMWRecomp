/* Core of the runtime: guest memory, PE image loading, call dispatch, HLE thunks,
 * guest heap, threads (run synchronously) and process start-up. */
#include "hle.h"
#include "js.h"
#include <stdarg.h>
#include <setjmp.h>
#include <emscripten.h>

CPU g_cpu;
gfn g_disp[TEXT_HI - TEXT_LO];
int g_log_level = 1;

extern const struct { uint32_t a; gfn f; } g_disp_list[];
extern const struct { uint32_t slot; const char *dll; const char *name; gfn f; } g_imports[];

/* ------------------------------------------------------------------ errors */
static void dump_state(void) {
  fprintf(stderr, "eax=%08x ecx=%08x edx=%08x ebx=%08x esp=%08x ebp=%08x esi=%08x edi=%08x\n",
          g_cpu.eax, g_cpu.ecx, g_cpu.edx, g_cpu.ebx, g_cpu.esp, g_cpu.ebp, g_cpu.esi, g_cpu.edi);
  /* walk the guest stack for plausible return addresses */
  fprintf(stderr, "stack:");
  uint32_t sp = g_cpu.esp;
  int n = 0;
  for (uint32_t k = 0; k < 4096 && n < 24; k += 4) {
    uint32_t a = sp + k;
    if (a >= 0x20000000u) break;
    uint32_t v = LD32(a);
    if (v >= TEXT_LO && v < TEXT_HI) { fprintf(stderr, " %06x", v); n++; }
  }
  fprintf(stderr, "\n");
}

void fatal(const char *fmt, ...) {
  char buf[1024];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof buf, fmt, ap);
  va_end(ap);
  fprintf(stderr, "FATAL: %s\n", buf);
  dump_state();
  js_fatal(buf);
  abort();
}

void guest_trap(uint32_t addr, const char *why) { fatal("guest trap at %08x: %s", addr, why); }
void bad_jump(uint32_t from, uint32_t to) { fatal("bad jump from %08x to %08x", from, to); }
void div_by_zero(uint32_t addr) { fatal("integer divide error at %08x", addr); }

/* ------------------------------------------------------------------ HLE thunks */
#define THUNK_MAX ((HLE_END - HLE_BASE) / 4)
static gfn thunk_fn[THUNK_MAX];
static const char *thunk_name[THUNK_MAX];
static uint32_t nthunks;

uint32_t hle_thunk(gfn f, const char *name) {
  for (uint32_t k = 0; k < nthunks; k++)
    if (thunk_fn[k] == f) return HLE_BASE + k * 4;
  if (nthunks >= THUNK_MAX) fatal("out of HLE thunks");
  uint32_t a = HLE_BASE + nthunks * 4;
  thunk_fn[nthunks] = f;
  thunk_name[nthunks] = name;
  nthunks++;
  ST32(a, 0xCCCCCCC3u); /* ret; int3 x3: harmless if anything inspects the bytes */
  return a;
}

const char *hle_thunk_name(uint32_t a) {
  if (a >= HLE_BASE && a < HLE_BASE + nthunks * 4) return thunk_name[(a - HLE_BASE) / 4];
  return NULL;
}

void call_slow(uint32_t t) {
  if (t >= HLE_BASE && t < HLE_BASE + nthunks * 4 && !(t & 3)) {
    thunk_fn[(t - HLE_BASE) / 4]();
    return;
  }
  fatal("call to unknown code address %08x (return to %08x)", t, LD32(g_cpu.esp));
}

/* Calls a guest function. Arguments are pushed right to left; for stdcall the
 * callee pops them, for cdecl we do. The guest stack pointer is restored either way. */
uint32_t guest_call(uint32_t addr, int nargs, const uint32_t *args, int cdecl) {
  (void)cdecl;
  uint32_t esp0 = g_cpu.esp;
  uint32_t esp = esp0 & ~3u;
  for (int k = nargs - 1; k >= 0; k--) { esp -= 4; ST32(esp, args[k]); }
  esp -= 4;
  ST32(esp, HLE_BASE); /* return address: never executed, calls are C calls */
  g_cpu.esp = esp;
  CALLI(addr);
  uint32_t r = g_cpu.eax;
  g_cpu.esp = esp0;
  return r;
}

/* ------------------------------------------------------------------ x87 / cpu helpers */
double ld80(uint32_t a) {
  uint64_t m = LD64(a);
  uint16_t se = LD16(a + 8);
  int e = se & 0x7fff;
  double r;
  if (e == 0 && m == 0) r = 0.0;
  else if (e == 0x7fff) r = (m << 1) ? NAN : INFINITY;
  else r = ldexp((double)m, e - 16383 - 63);
  return (se & 0x8000) ? -r : r;
}

void st80(uint32_t a, double v) {
  uint16_t se = signbit(v) ? 0x8000 : 0;
  uint64_t m = 0;
  if (isnan(v)) { se |= 0x7fff; m = 0xC000000000000000ull; }
  else if (isinf(v)) { se |= 0x7fff; m = 0x8000000000000000ull; }
  else if (v != 0.0) {
    int e;
    double f = frexp(fabs(v), &e); /* v = f * 2^e, f in [0.5,1) */
    m = (uint64_t)ldexp(f, 64);
    se |= (uint16_t)(e - 1 + 16383);
  }
  ST64(a, m);
  ST16(a + 8, se);
}

double fxam_dummy(double v) { return v; }

uint32_t fxam_sw(uint32_t fsw, double v, int empty) {
  fsw &= ~0x4700u;
  if (signbit(v)) fsw |= 0x200;
  if (empty) return fsw | 0x4100;
  if (isnan(v)) return fsw | 0x100;
  if (isinf(v)) return fsw | 0x500;
  if (v == 0.0) return fsw | 0x4000;
  if (fpclassify(v) == FP_SUBNORMAL) return fsw | 0x4400;
  return fsw | 0x400;
}

void do_cpuid(uint32_t *a, uint32_t *b, uint32_t *c, uint32_t *d) {
  /* Pentium-class CPU without MMX/SSE so the CRT keeps to its x87 paths. */
  if (*a == 0) { *a = 1; *b = 0x756e6547; *d = 0x49656e69; *c = 0x6c65746e; return; }
  if (*a == 1) { *a = 0x00000f29; *b = 0; *c = 0; *d = 0x00008111; return; }
  *a = *b = *c = *d = 0;
}

/* The game's clock: real time, optionally running faster (set_time_warp, from the page) so
   screens that just wait a number of frames go by quickly (the game's own frame limiter reads
   this clock; the page also stops waiting for the display then: Module.turbo). */
static double warp = 1, warp_real0, warp_virt0;
static double game_now(void) {
  double r = emscripten_get_now();
  return warp_virt0 + (r - warp_real0) * warp;
}
EMSCRIPTEN_KEEPALIVE void set_time_warp(double w) {
  if (!(w >= 1 && w <= 20)) w = 1;
  double v = game_now();
  warp_real0 = emscripten_get_now();
  warp_virt0 = v;
  warp = w;
}

uint64_t do_rdtsc(void) { return (uint64_t)(game_now() * 1e6); }

uint32_t host_now_ms(void) { return (uint32_t)(uint64_t)game_now(); }
double host_now_us(void) { return game_now() * 1000.0; }

/* static allocations in the low "system" area (never freed) */
static uint32_t sys_brk = SYS_AREA_LO;
uint32_t sys_alloc(uint32_t n) {
  uint32_t p = (sys_brk + 15) & ~15u;
  if (p + n > SYS_AREA_HI) fatal("system area exhausted");
  sys_brk = p + n;
  memset(GPTR(p), 0, n);
  return p;
}
uint32_t sys_strdup(const char *s) {
  uint32_t n = (uint32_t)strlen(s) + 1, p = sys_alloc(n);
  memcpy(GPTR(p), s, n);
  return p;
}

/* ------------------------------------------------------------------ threads */
/* Win32 threads are run to completion synchronously when started (the game only
 * uses a worker thread for loading sounds). Each thread gets its own TEB contents,
 * TLS slots and guest stack. */
Thread g_threads[MAX_THREADS];
Thread *g_cur_thread;

uint32_t cur_thread_id(void) { return g_cur_thread->id; }

static void teb_init(Thread *t) {
  memset(t->teb, 0, sizeof t->teb);
  uint32_t *w = (uint32_t *)t->teb;
  w[0] = 0xFFFFFFFFu;          /* SEH chain end */
  w[1] = t->stack_hi;          /* stack base */
  w[2] = t->stack_lo;          /* stack limit */
  w[0x18 / 4] = FS_BASE;       /* self */
  w[0x20 / 4] = 1;             /* process id */
  w[0x24 / 4] = t->id;         /* thread id */
  w[0x30 / 4] = PEB_ADDR;
}

Thread *thread_new(uint32_t start, uint32_t param, uint32_t stack_size) {
  for (int k = 1; k < MAX_THREADS; k++) {
    Thread *t = &g_threads[k];
    if (t->used) continue;
    memset(t, 0, sizeof *t);
    t->used = 1;
    t->id = 0x100 + k * 4;
    t->start = start;
    t->param = param;
    if (stack_size < 0x40000) stack_size = 0x40000;
    t->stack_lo = gmemalign(16, stack_size);
    if (!t->stack_lo) fatal("thread stack alloc failed");
    t->stack_hi = t->stack_lo + stack_size;
    teb_init(t);
    return t;
  }
  fatal("too many threads");
}

void thread_run(Thread *t) {
  if (t->started) return;
  t->started = 1;
  Thread *prev = g_cur_thread;
  CPU saved = g_cpu;
  memcpy(prev->teb, GPTR(FS_BASE), sizeof prev->teb);
  memcpy(GPTR(FS_BASE), t->teb, sizeof t->teb);
  g_cur_thread = t;
  g_cpu.esp = t->stack_hi - 16;
  g_cpu.ebp = 0;
  if (setjmp(t->exit_jmp) == 0) {
    uint32_t r = guest_call(t->start, 1, &t->param, 0);
    t->exit_code = r;
  }
  t->done = 1;
  memcpy(t->teb, GPTR(FS_BASE), sizeof t->teb);
  memcpy(GPTR(FS_BASE), prev->teb, sizeof prev->teb);
  g_cur_thread = prev;
  g_cpu = saved;
  gfree(t->stack_lo);
  t->stack_lo = 0;
}

void thread_exit(uint32_t code) {
  Thread *t = g_cur_thread;
  if (t == &g_threads[0]) process_exit(code);
  t->exit_code = code;
  longjmp(t->exit_jmp, 1);
}

/* ------------------------------------------------------------------ process */
static jmp_buf exit_jmp;
static uint32_t exit_code;

void process_exit(uint32_t code) {
  exit_code = code;
  longjmp(exit_jmp, 1);
}

static void load_image(void) {
  int fd = js_fopen("PMR.exe", 0);
  if (fd < 0) fatal("cannot open PMR.exe");
  uint32_t size = (uint32_t)js_fsize(fd);
  uint8_t *buf = malloc(size);
  js_fread(fd, 0, buf, size);
  js_fclose(fd);
  uint32_t pe = *(uint32_t *)(buf + 0x3c);
  if (memcmp(buf + pe, "PE\0\0", 4)) fatal("PMR.exe: bad PE signature");
  uint16_t nsec = *(uint16_t *)(buf + pe + 6);
  uint16_t optsz = *(uint16_t *)(buf + pe + 20);
  uint8_t *opt = buf + pe + 24;
  uint32_t base = *(uint32_t *)(opt + 28);
  uint32_t imgsize = *(uint32_t *)(opt + 56);
  uint32_t hdrsize = *(uint32_t *)(opt + 60);
  if (base != IMAGE_BASE) fatal("PMR.exe: unexpected image base %08x", base);
  if (IMAGE_BASE + imgsize > IMAGE_END) fatal("PMR.exe: image too large");
  memset(GPTR(IMAGE_BASE), 0, imgsize);
  memcpy(GPTR(IMAGE_BASE), buf, hdrsize);
  uint8_t *sh = opt + optsz;
  for (int k = 0; k < nsec; k++, sh += 40) {
    uint32_t vsize = *(uint32_t *)(sh + 8), va = *(uint32_t *)(sh + 12);
    uint32_t rawsize = *(uint32_t *)(sh + 16), rawptr = *(uint32_t *)(sh + 20);
    uint32_t n = rawsize < vsize || !vsize ? rawsize : vsize;
    if (rawptr + n > size) n = size > rawptr ? size - rawptr : 0;
    memcpy(GPTR(IMAGE_BASE + va), buf + rawptr, n);
  }
  free(buf);
  /* imports: point every IAT slot at its HLE thunk */
  for (int k = 0; g_imports[k].slot; k++) {
    char nm[160];
    snprintf(nm, sizeof nm, "%s!%s", g_imports[k].dll, g_imports[k].name);
    ST32(g_imports[k].slot, hle_thunk(g_imports[k].f, strdup(nm)));
  }
}

uint32_t g_entry_point;

char g_extra_args[256];

int main(int argc, char **argv) {
  for (int k = 1; k < argc; k++) {
    strncat(g_extra_args, " ", sizeof g_extra_args - strlen(g_extra_args) - 1);
    strncat(g_extra_args, argv[k], sizeof g_extra_args - strlen(g_extra_args) - 1);
  }
  js_init();
  for (int k = 0; g_disp_list[k].a; k++) g_disp[g_disp_list[k].a - TEXT_LO] = g_disp_list[k].f;
  memset(GPTR(0x1000), 0, GUEST_HEAP_LO - 0x1000);
  gheap_init();
  load_image();
  {
    uint8_t *opt = (uint8_t *)GPTR(IMAGE_BASE + LD32(IMAGE_BASE + 0x3c) + 24);
    g_entry_point = IMAGE_BASE + *(uint32_t *)(opt + 16);
  }
  /* PEB: ImageBaseAddress at +8, ProcessHeap at +0x18 filled by kernel32 */
  ST32(PEB_ADDR + 8, IMAGE_BASE);
  Thread *t = &g_threads[0];
  t->used = 1;
  t->started = 1;
  t->id = 0x100;
  t->stack_lo = MAIN_STACK_LO;
  t->stack_hi = MAIN_STACK_HI;
  teb_init(t);
  memcpy(GPTR(FS_BASE), t->teb, sizeof t->teb);
  g_cur_thread = t;
  win32_init();
  g_cpu.esp = MAIN_STACK_HI - 64;
  g_cpu.fcw = 0x27f;
  if (setjmp(exit_jmp) == 0) {
    uint32_t r = guest_call(g_entry_point, 0, NULL, 0);
    exit_code = r;
  }
  fprintf(stderr, "process exited with code %u\n", exit_code);
  js_exit((int)exit_code);
  return (int)exit_code;
}
