/* Host services implemented in JavaScript (runtime/lib.js). Functions marked
 * async suspend the wasm stack with JSPI while the browser does the work. */
#pragma once
#include <stdint.h>

void js_init(void);
void js_fatal(const char *msg);
void js_exit(int code);

/* files: paths are game-relative with forward slashes, e.g. "streams/Win32/Award.ogg"
 * or "save/<name>" for the persistent save area. */
int js_fopen(const char *path, int mode);          /* async; mode 0=read 1=create/truncate 2=rw existing 3=rw always; -1 on failure */
int js_fsize(int fd);
int js_fread(int fd, uint32_t off, void *dst, int len);
int js_fwrite(int fd, uint32_t off, const void *src, int len);
void js_fsetsize(int fd, uint32_t size);
void js_fclose(int fd);
int js_fexists(const char *path);                  /* 0 none, 1 file, 2 directory */
int js_fdelete(const char *path);
void js_mkdir(const char *path);

/* frame pacing / time */
void js_present(void);                             /* async: waits for the next animation frame */
void js_sleep(int ms);                             /* async */
void js_yield(void);                               /* async: lets the browser process events */
int js_canvas_width(void);
int js_canvas_height(void);
void js_set_canvas_size(int w, int h);

/* input events: each event is 4 int32 {type, a, b, c}; returns number of events written */
int js_poll_events(int32_t *buf, int max);
int js_gamepad_state(int index, int32_t *axes6, uint32_t *buttons, int32_t *pov); /* 0 if not connected */
int js_gamepad_count(void);
void js_set_cursor_visible(int v);
void js_message_box(const char *text, const char *caption);
