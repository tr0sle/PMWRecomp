/* Shared state of the Win32 emulation (kernel32/user32/gdi32/...). */
#pragma once
#include "hle.h"

#define HANDLE_BASE 0x00001000u
#define MAX_HANDLES 1024
enum { H_FREE, H_FILE, H_MUTEX, H_THREAD, H_HEAP, H_STD, H_EVENT };
typedef struct {
  int type;
  int fd;
  uint32_t pos;
  Thread *thread;
} Handle;
extern Handle g_handles[MAX_HANDLES];
uint32_t handle_new(int type);
Handle *handle_get(uint32_t h, int type);
void set_last_error(uint32_t e);

#define GAME_CMDLINE "\"C:\\PMWR\\PMR.exe\""
#define SAVE_ROOT "C:\\Users\\Player\\Documents"
#define SAVE_ROOT_LOW "c:/users/player/documents/"
void path_to_host(const char *in, char *out, int outsz);

/* Lets the browser run (input, audio callbacks) when the game busy-waits. */
void maybe_yield(void);
void note_yield(void);

/* HLE function lookup by module and export name (GetProcAddress, wglGetProcAddress). */
uint32_t hle_lookup(const char *module, const char *name);

void kernel32_init(void);
void user32_init(void);
void gl_init(void);
void gl_present(void);

/* windows / messages */
#define MAIN_HWND 0x00010001u
void post_message(uint32_t hwnd, uint32_t msg, uint32_t wp, uint32_t lp);
void pump_host_events(void);
extern uint8_t g_keys[256];     /* DirectInput scan-code state (0x80 = down) */
extern int32_t g_mouse_dx, g_mouse_dy, g_mouse_dz;
extern uint8_t g_mouse_buttons[4];
