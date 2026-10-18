/* Guest CPU model shared by the recompiled code (gen/code_NNN.c) and the HLE runtime.
 * Guest memory is identity-mapped into the wasm linear memory: guest address A
 * lives at wasm address A (emscripten data starts at GLOBAL_BASE, above the guest). */
#pragma once
#include <stdint.h>
#include <string.h>
#include <math.h>

#ifdef NATIVE
extern uint8_t *g_membase;
#define GPTR(a) ((void *)(g_membase + (uint32_t)(a)))
#define HADDR(p) ((uint32_t)((uint8_t *)(p) - g_membase))
#else
#define GPTR(a) ((void *)(uintptr_t)(uint32_t)(a))
#define HADDR(p) ((uint32_t)(uintptr_t)(p))
#endif

typedef uint16_t __attribute__((aligned(1), may_alias)) u16u;
typedef uint32_t __attribute__((aligned(1), may_alias)) u32u;
typedef uint64_t __attribute__((aligned(1), may_alias)) u64u;
typedef float __attribute__((aligned(1), may_alias)) f32u;
typedef double __attribute__((aligned(1), may_alias)) f64u;

#define LD8(a) (*(uint8_t *)GPTR(a))
#define LD16(a) (*(u16u *)GPTR(a))
#define LD32(a) (*(u32u *)GPTR(a))
#define LD64(a) (*(u64u *)GPTR(a))
#define LDF32(a) ((double)*(f32u *)GPTR(a))
#define LDF64(a) (*(f64u *)GPTR(a))
#define ST8(a, v) (*(uint8_t *)GPTR(a) = (uint8_t)(v))
#define ST16(a, v) (*(u16u *)GPTR(a) = (uint16_t)(v))
#define ST32(a, v) (*(u32u *)GPTR(a) = (uint32_t)(v))
#define ST64(a, v) (*(u64u *)GPTR(a) = (uint64_t)(v))
#define STF32(a, v) (*(f32u *)GPTR(a) = (float)(v))
#define STF64(a, v) (*(f64u *)GPTR(a) = (double)(v))

/* guest memory map */
#define FS_BASE 0x00030000u      /* TEB */
#define IMAGE_BASE 0x00400000u
#define TEXT_LO 0x00401000u
#define TEXT_HI 0x004F8000u
#define HLE_BASE 0x00020000u     /* thunk addresses for imported/HLE functions (4 bytes each) */
#define HLE_END 0x00030000u

typedef struct {
  uint32_t eax, ecx, edx, ebx, esp, ebp, esi, edi;
  uint32_t df;      /* direction flag */
  uint32_t eflags_extra;
  double st[8];
  int ftop;
  uint32_t fcw;
} CPU;
extern CPU g_cpu;

#define ST(i) (g_cpu.st[(g_cpu.ftop + (i)) & 7])
#define FPUSH(v) do { double _v = (v); g_cpu.ftop = (g_cpu.ftop - 1) & 7; g_cpu.st[g_cpu.ftop] = _v; } while (0)
#define FPOP() (g_cpu.ftop = (g_cpu.ftop + 1) & 7)

#define SYNC_OUT g_cpu.eax = eax; g_cpu.ecx = ecx; g_cpu.edx = edx; g_cpu.ebx = ebx; g_cpu.esp = esp; g_cpu.ebp = ebp; g_cpu.esi = esi; g_cpu.edi = edi
#define SYNC_IN eax = g_cpu.eax; ecx = g_cpu.ecx; edx = g_cpu.edx; ebx = g_cpu.ebx; esp = g_cpu.esp; ebp = g_cpu.ebp; esi = g_cpu.esi; edi = g_cpu.edi
#define REGS_DECL uint32_t eax = g_cpu.eax, ecx = g_cpu.ecx, edx = g_cpu.edx, ebx = g_cpu.ebx, esp = g_cpu.esp, ebp = g_cpu.ebp, esi = g_cpu.esi, edi = g_cpu.edi

typedef void (*gfn)(void);
extern gfn g_disp[];   /* indexed by addr - TEXT_LO */
void call_slow(uint32_t target);
static inline void CALLI(uint32_t t) {
  uint32_t o = t - TEXT_LO;
  if (o < (TEXT_HI - TEXT_LO) && g_disp[o]) g_disp[o]();
  else call_slow(t);
}

void guest_trap(uint32_t addr, const char *why) __attribute__((noreturn));
#define TRAP(a, why) guest_trap(a, why)
void bad_jump(uint32_t from, uint32_t to) __attribute__((noreturn));

static inline uint32_t PARITY(uint32_t v) { return !__builtin_parity(v & 0xff); }

/* ---------------- x87 helpers */
static inline double fround(double v) {
  switch ((g_cpu.fcw >> 10) & 3) {
    case 0: return nearbyint(v);
    case 1: return floor(v);
    case 2: return ceil(v);
    default: return trunc(v);
  }
}
static inline uint32_t fist32(double v) {
  v = fround(v);
  if (!(v >= -2147483648.0 && v < 2147483648.0)) return 0x80000000u;
  return (uint32_t)(int32_t)v;
}
static inline uint32_t fist16(double v) {
  v = fround(v);
  if (!(v >= -32768.0 && v < 32768.0)) return 0x8000u;
  return (uint32_t)(uint16_t)(int16_t)v;
}
static inline uint64_t fist64(double v) {
  v = fround(v);
  if (!(v >= -9223372036854775808.0 && v < 9223372036854775808.0)) return 0x8000000000000000ull;
  return (uint64_t)(int64_t)v;
}
static inline uint64_t ftrunc64(double v) {
  v = trunc(v);
  if (!(v >= -9223372036854775808.0 && v < 9223372036854775808.0)) return 0x8000000000000000ull;
  return (uint64_t)(int64_t)v;
}
static inline uint32_t fcmp_sw(uint32_t fsw, double a, double b) {
  fsw &= ~0x4500u;
  if (a > b) return fsw;
  if (a < b) return fsw | 0x0100u;
  if (a == b) return fsw | 0x4000u;
  return fsw | 0x4500u;
}
double ld80(uint32_t addr);
void st80(uint32_t addr, double v);
double fxam_dummy(double);
uint32_t fxam_sw(uint32_t fsw, double v, int empty);

/* ---------------- misc helpers */
void do_cpuid(uint32_t *a, uint32_t *b, uint32_t *c, uint32_t *d);
uint64_t do_rdtsc(void);
void div_by_zero(uint32_t addr) __attribute__((noreturn));

static inline void rep_stos(uint32_t *edi, uint32_t *ecx, uint32_t val, int sz) {
  uint32_t n = *ecx;
  if (!n) return;
  if (!g_cpu.df) {
    if (sz == 1) memset(GPTR(*edi), (int)(val & 0xff), n);
    else if (sz == 4 && val == 0) memset(GPTR(*edi), 0, (size_t)n * 4);
    else {
      uint32_t a = *edi;
      for (uint32_t k = 0; k < n; k++, a += sz) {
        if (sz == 4) ST32(a, val); else ST16(a, val);
      }
    }
    *edi += n * sz;
  } else {
    uint32_t a = *edi;
    for (uint32_t k = 0; k < n; k++, a -= sz) {
      if (sz == 4) ST32(a, val); else if (sz == 2) ST16(a, val); else ST8(a, val);
    }
    *edi = a;
  }
  *ecx = 0;
}
static inline void rep_movs(uint32_t *edi, uint32_t *esi, uint32_t *ecx, int sz) {
  uint32_t n = *ecx;
  if (!n) return;
  uint32_t bytes = n * sz;
  if (!g_cpu.df) {
    uint32_t d = *edi, s = *esi;
    if (d <= s || d >= s + bytes) memmove(GPTR(d), GPTR(s), bytes);
    else { for (uint32_t k = 0; k < bytes; k++) ST8(d + k, LD8(s + k)); }
    *edi += bytes; *esi += bytes;
  } else {
    uint32_t d = *edi, s = *esi;
    for (uint32_t k = 0; k < n; k++, d -= sz, s -= sz) {
      if (sz == 4) ST32(d, LD32(s)); else if (sz == 2) ST16(d, LD16(s)); else ST8(d, LD8(s));
    }
    *edi = d; *esi = s;
  }
  *ecx = 0;
}
