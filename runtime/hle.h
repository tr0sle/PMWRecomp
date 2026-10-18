/* Helpers for high-level emulation (HLE) of the Win32 / OpenGL / FMOD / DirectInput
 * APIs imported by PMR.exe. HLE functions are called with the guest return address
 * on top of the guest stack (g_cpu.esp) and arguments above it (stdcall). */
#pragma once
#include "cpu.h"
#include <stdio.h>
#include <stdlib.h>

#define ARG(n) LD32(g_cpu.esp + 4 + 4 * (n))
#define ARGF(n) (*(f32u *)GPTR(g_cpu.esp + 4 + 4 * (n)))
#define ARGD(n) (*(f64u *)GPTR(g_cpu.esp + 4 + 4 * (n)))
#define ARGP(n) ((char *)GPTR(ARG(n)))
/* stdcall return: pop return address + nargs dwords */
#define RET(nargs, v) do { g_cpu.eax = (uint32_t)(v); g_cpu.esp += 4 + 4 * (nargs); return; } while (0)
#define RETV(nargs) do { g_cpu.esp += 4 + 4 * (nargs); return; } while (0)

#define HLE(dll, name) void hle_##dll##_##name(void)

extern int g_log_level;
#define LOG(...) do { if (g_log_level > 0) { fprintf(stderr, __VA_ARGS__); } } while (0)
#define LOGV(...) do { if (g_log_level > 1) { fprintf(stderr, __VA_ARGS__); } } while (0)

/* guest heap (dlmalloc mspace over the guest address range) */
uint32_t gmalloc(uint32_t size);
uint32_t gcalloc(uint32_t size);
uint32_t gmemalign(uint32_t align, uint32_t size);
void gfree(uint32_t p);
uint32_t grealloc(uint32_t p, uint32_t size);
uint32_t gusable(uint32_t p);
uint32_t gstrdup(const char *s);

/* guest call helpers: call guest function at addr with stdcall/cdecl args */
uint32_t guest_call(uint32_t addr, int nargs, const uint32_t *args, int cdecl);

/* HLE thunk addresses for host functions callable by the guest (vtables, GetProcAddress) */
uint32_t hle_thunk(gfn f, const char *name);

void fatal(const char *fmt, ...) __attribute__((noreturn));
uint32_t host_now_ms(void);
double host_now_us(void);

/* threads */
uint32_t cur_thread_id(void);
uint32_t sys_alloc(uint32_t n);
uint32_t sys_strdup(const char *s);
void gheap_init(void);
const char *hle_thunk_name(uint32_t a);

/* ---------------- guest memory map (guest address == wasm address) */
#define SYS_AREA_LO 0x00010000u    /* static runtime data (strings, tables) */
#define SYS_AREA_HI 0x00020000u
#define PEB_ADDR 0x00031000u
#define MAIN_STACK_LO 0x00100000u
#define MAIN_STACK_HI 0x003F0000u
#define IMAGE_END 0x00900000u
#define GUEST_HEAP_LO 0x00900000u
#define GUEST_HEAP_HI 0x1FF00000u  /* emscripten's own data starts at 512MB */

/* ---------------- threads (run synchronously) */
#include <setjmp.h>
#define MAX_THREADS 16
#define TLS_SLOTS 64
typedef struct {
  int used, started, done;
  uint32_t id, start, param, exit_code;
  uint32_t stack_lo, stack_hi;
  uint32_t tls[TLS_SLOTS];
  uint8_t teb[0x100];
  jmp_buf exit_jmp;
} Thread;
extern Thread g_threads[MAX_THREADS];
extern Thread *g_cur_thread;
Thread *thread_new(uint32_t start, uint32_t param, uint32_t stack_size);
void thread_run(Thread *t);
void thread_exit(uint32_t code) __attribute__((noreturn));
void process_exit(uint32_t code) __attribute__((noreturn));
void win32_init(void);
