/* DirectInput 8 emulation: COM objects whose vtables point at HLE thunks.
 * Keyboard and mouse come from browser events, joysticks from the Gamepad API
 * (standard mapping, presented like an XInput pad through DirectInput). */
#include "hle.h"
#include "js.h"
#include "win32.h"
#include <emscripten.h>

#define DI_OK 0
#define DI_NOEFFECT 1
#define DIERR_INVALIDPARAM 0x80070057u
#define DIERR_NOTACQUIRED 0x8007000Cu
#define DIERR_DEVICENOTREG 0x80040154u
#define DIERR_UNSUPPORTED 0x80004001u
#define E_NOINTERFACE 0x80004002u

enum { DEV_KEYBOARD = 1, DEV_MOUSE = 2, DEV_JOY = 3 };

/* device object (axis/button/pov) description */
typedef struct { uint32_t guid1; int kind; int index; const char *name; } DevObj; /* kind: 0 axis 1 button 2 pov */
enum { OBJ_AXIS, OBJ_BUTTON, OBJ_POV };

#define G_XAXIS 0xA36D02E0u
#define G_YAXIS 0xA36D02E1u
#define G_ZAXIS 0xA36D02E2u
#define G_RZAXIS 0xA36D02E3u
#define G_SLIDER 0xA36D02E4u
#define G_BUTTON 0xA36D02F0u
#define G_KEY 0x55728220u
#define G_POV 0xA36D02F2u
#define G_RXAXIS 0xA36D02F4u
#define G_RYAXIS 0xA36D02F5u

static const DevObj joy_objs[] = {
  {G_XAXIS, OBJ_AXIS, 0, "X Axis"}, {G_YAXIS, OBJ_AXIS, 1, "Y Axis"}, {G_ZAXIS, OBJ_AXIS, 2, "Z Axis"},
  {G_RXAXIS, OBJ_AXIS, 3, "X Rotation"}, {G_RYAXIS, OBJ_AXIS, 4, "Y Rotation"},
  {G_BUTTON, OBJ_BUTTON, 0, "Button 0"}, {G_BUTTON, OBJ_BUTTON, 1, "Button 1"}, {G_BUTTON, OBJ_BUTTON, 2, "Button 2"},
  {G_BUTTON, OBJ_BUTTON, 3, "Button 3"}, {G_BUTTON, OBJ_BUTTON, 4, "Button 4"}, {G_BUTTON, OBJ_BUTTON, 5, "Button 5"},
  {G_BUTTON, OBJ_BUTTON, 6, "Button 6"}, {G_BUTTON, OBJ_BUTTON, 7, "Button 7"}, {G_BUTTON, OBJ_BUTTON, 8, "Button 8"},
  {G_BUTTON, OBJ_BUTTON, 9, "Button 9"}, {G_POV, OBJ_POV, 0, "Hat Switch"},
};
#define NJOY_AXES 5
#define NJOY_BUTTONS 10
static const DevObj mouse_objs[] = {
  {G_XAXIS, OBJ_AXIS, 0, "X-axis"}, {G_YAXIS, OBJ_AXIS, 1, "Y-axis"}, {G_ZAXIS, OBJ_AXIS, 2, "Wheel"},
  {G_BUTTON, OBJ_BUTTON, 0, "Button 0"}, {G_BUTTON, OBJ_BUTTON, 1, "Button 1"},
  {G_BUTTON, OBJ_BUTTON, 2, "Button 2"}, {G_BUTTON, OBJ_BUTTON, 3, "Button 3"},
};

typedef struct { uint32_t ofs; int kind, index; } Mapped;

#define MAX_DEVS 12
#define BUFSZ 256
typedef struct {
  int used, type, pad, acquired;
  uint32_t guest;              /* guest COM object */
  uint32_t data_size;
  int nmapped;
  Mapped map[300];
  int32_t range_min[8], range_max[8], deadzone[8];
  uint32_t buffer_size;
  /* buffered data */
  struct { uint32_t ofs, data, time, seq; } buf[BUFSZ];
  int bhead, btail;
  /* last joystick state for buffered change detection */
  int32_t last_axes[8];
  uint32_t last_buttons;
  int32_t last_pov;
} Dev;
static Dev devs[MAX_DEVS];
static uint32_t di_seq;
static uint32_t vtbl_di, vtbl_dev, di_obj;

static Dev *dev_of(uint32_t this) {
  uint32_t k = LD32(this + 4);
  if (k >= MAX_DEVS || !devs[k].used) fatal("DirectInput: bad device object %08x", this);
  return &devs[k];
}

static void buf_push(Dev *d, uint32_t ofs, uint32_t data) {
  if (!d->buffer_size || !d->acquired) return;
  int n = (d->btail + 1) % BUFSZ;
  if (n == d->bhead) d->bhead = (d->bhead + 1) % BUFSZ; /* overflow: drop oldest */
  d->buf[d->btail].ofs = ofs;
  d->buf[d->btail].data = data;
  d->buf[d->btail].time = host_now_ms();
  d->buf[d->btail].seq = ++di_seq;
  d->btail = n;
}

static int find_mapped(Dev *d, int kind, int index) {
  for (int k = 0; k < d->nmapped; k++)
    if (d->map[k].kind == kind && d->map[k].index == index) return (int)d->map[k].ofs;
  return -1;
}

void dinput_key_event(int scan, int down) {
  for (int k = 0; k < MAX_DEVS; k++)
    if (devs[k].used && devs[k].type == DEV_KEYBOARD) {
      int ofs = devs[k].nmapped ? find_mapped(&devs[k], OBJ_BUTTON, scan) : scan;
      if (ofs >= 0) buf_push(&devs[k], (uint32_t)ofs, down ? 0x80 : 0);
    }
}

void dinput_mouse_event(int type, int a, int b) {
  for (int k = 0; k < MAX_DEVS; k++) {
    Dev *d = &devs[k];
    if (!d->used || d->type != DEV_MOUSE) continue;
    if (type == 0) {
      int ox = find_mapped(d, OBJ_AXIS, 0), oy = find_mapped(d, OBJ_AXIS, 1);
      if (a && ox >= 0) buf_push(d, ox, (uint32_t)a);
      if (b && oy >= 0) buf_push(d, oy, (uint32_t)b);
    } else if (type == 1) {
      int o = find_mapped(d, OBJ_BUTTON, a);
      if (o >= 0) buf_push(d, o, b ? 0x80 : 0);
    } else {
      int o = find_mapped(d, OBJ_AXIS, 2);
      if (o >= 0) buf_push(d, o, (uint32_t)a);
    }
  }
}

/* ------------------------------------------------------------------ helpers */
static void write_guid(uint32_t p, uint32_t d1, uint16_t d2, uint16_t d3, const uint8_t d4[8]) {
  ST32(p, d1); ST16(p + 4, d2); ST16(p + 6, d3);
  memcpy(GPTR(p + 8), d4, 8);
}
static const uint8_t SYS_D4[8] = {0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00};
static const uint8_t PAD_D4[8] = {0x00, 0x00, 0x50, 0x49, 0x44, 0x56, 0x49, 0x44};

static int guid_is(uint32_t p, uint32_t d1) { return p && LD32(p) == d1; }

#define MAX_PADS 4
static int joy_count(void) { return js_gamepad_count(); }

static uint32_t new_device(int type, int pad) {
  for (int k = 0; k < MAX_DEVS; k++) {
    if (devs[k].used) continue;
    Dev *d = &devs[k];
    memset(d, 0, sizeof *d);
    d->used = 1;
    d->type = type;
    d->pad = pad;
    for (int a = 0; a < 8; a++) { d->range_min[a] = 0; d->range_max[a] = 65535; }
    d->last_pov = -1;
    d->guest = sys_alloc(8);
    ST32(d->guest, vtbl_dev);
    ST32(d->guest + 4, (uint32_t)k);
    return d->guest;
  }
  fatal("DirectInput: too many devices");
}

static void fill_devinst(uint32_t p, int type, int pad) {
  uint32_t sz = LD32(p);
  memset(GPTR(p + 4), 0, (sz ? sz : 580) - 4);
  char name[64];
  if (type == DEV_KEYBOARD) {
    write_guid(p + 4, 0x6F1D2B61, 0xD5A0, 0x11CF, SYS_D4);
    write_guid(p + 20, 0x6F1D2B61, 0xD5A0, 0x11CF, SYS_D4);
    ST32(p + 36, 0x0413);
    strcpy(name, "Keyboard");
  } else if (type == DEV_MOUSE) {
    write_guid(p + 4, 0x6F1D2B60, 0xD5A0, 0x11CF, SYS_D4);
    write_guid(p + 20, 0x6F1D2B60, 0xD5A0, 0x11CF, SYS_D4);
    ST32(p + 36, 0x0112);
    strcpy(name, "Mouse");
  } else {
    write_guid(p + 4, 0x9E573ED0u + pad, 0x7734, 0x11D2, SYS_D4);
    write_guid(p + 20, 0x028E045Eu, 0, 0, PAD_D4); /* Xbox 360 controller product GUID */
    ST32(p + 36, 0x00010215);
    snprintf(name, sizeof name, "Controller (Gamepad %d)", pad + 1);
  }
  strcpy((char *)GPTR(p + 40), name);
  strcpy((char *)GPTR(p + 300), name);
  if (type == DEV_JOY) { ST16(p + 576, 1); ST16(p + 578, 5); }
}

/* ------------------------------------------------------------------ IDirectInput8A */
HLE(dinput8, DirectInput8Create) { /* (hinst, ver, riid, ppv, outer) */
  ST32(ARG(3), di_obj);
  RET(5, DI_OK);
}

static void di_QueryInterface(void) { ST32(ARG(2), ARG(0)); RET(3, DI_OK); }
static void di_AddRef(void) { RET(1, 2); }
static void di_Release(void) { RET(1, 1); }

static void di_CreateDevice(void) { /* (this, guid, ppdev, outer) */
  uint32_t g = ARG(1), pp = ARG(2);
  uint32_t d1 = LD32(g);
  uint32_t obj;
  if (d1 == 0x6F1D2B61) obj = new_device(DEV_KEYBOARD, 0);
  else if (d1 == 0x6F1D2B60) obj = new_device(DEV_MOUSE, 0);
  else if ((d1 & ~0xfu) == 0x9E573ED0u) obj = new_device(DEV_JOY, (int)(d1 & 0xf));
  else { LOG("DirectInput: CreateDevice unknown guid %08x\n", d1); RET(4, DIERR_DEVICENOTREG); }
  LOG("DirectInput: CreateDevice %08x -> %08x\n", d1, obj);
  ST32(pp, obj);
  RET(4, DI_OK);
}

static void di_EnumDevices(void) { /* (this, type, cb, ref, flags) */
  uint32_t type = ARG(1) & 0xff, cb = ARG(2), ref = ARG(3);
  uint32_t inst = sys_alloc(580);
  int want_kb = type == 0 || type == 3 || type == 0x13;
  int want_mouse = type == 0 || type == 2 || type == 0x12;
  int want_joy = type == 0 || type == 4 || type == 0x14 || type == 0x15;
  LOG("DirectInput: EnumDevices type=%u flags=%x gamepads=%d\n", ARG(1), ARG(4), joy_count());
  int cont = 1;
  if (want_kb && cont) { ST32(inst, 580); fill_devinst(inst, DEV_KEYBOARD, 0); uint32_t a[2] = {inst, ref}; cont = guest_call(cb, 2, a, 0) != 0; }
  if (want_mouse && cont) { ST32(inst, 580); fill_devinst(inst, DEV_MOUSE, 0); uint32_t a[2] = {inst, ref}; cont = guest_call(cb, 2, a, 0) != 0; }
  if (want_joy) {
    /* The game enumerates controllers once, at start-up, but browsers only expose a gamepad
     * after a button is pressed on it with the page open. So 4 joysticks are always attached;
     * joystick k reads the k-th connected browser gamepad (neutral while there is none). */
    int n = joy_count() > MAX_PADS ? joy_count() : MAX_PADS;
    for (int k = 0; k < n && cont; k++) {
      ST32(inst, 580);
      fill_devinst(inst, DEV_JOY, k);
      uint32_t a[2] = {inst, ref};
      cont = guest_call(cb, 2, a, 0) != 0;
    }
  }
  RET(5, DI_OK);
}

static void di_GetDeviceStatus(void) { RET(2, DI_OK); }
static void di_RunControlPanel(void) { RET(3, DI_OK); }
static void di_Initialize(void) { RET(3, DI_OK); }
static void di_FindDevice(void) { RET(4, DIERR_DEVICENOTREG); }
static void di_EnumDevicesBySemantics(void) { RET(6, DIERR_UNSUPPORTED); }
static void di_ConfigureDevices(void) { RET(5, DIERR_UNSUPPORTED); }

/* ------------------------------------------------------------------ IDirectInputDevice8A */
static const DevObj *dev_objs(Dev *d, int *n) {
  if (d->type == DEV_JOY) { *n = (int)(sizeof joy_objs / sizeof joy_objs[0]); return joy_objs; }
  if (d->type == DEV_MOUSE) { *n = (int)(sizeof mouse_objs / sizeof mouse_objs[0]); return mouse_objs; }
  *n = 0;
  return NULL;
}

static int jcalls[32];
static const char *jnames[32] = {"QI", "AddRef", "Release", "GetCaps", "EnumObjects", "GetProp", "SetProp", "Acquire", "Unacquire",
  "GetState", "GetData", "SetFormat", "SetEvent", "SetCoop", "GetObjInfo", "GetDevInfo", "Panel", "Init", "CreateEffect",
  "EnumEffects", "GetEffectInfo", "GetFFState", "SendFF", "EnumCreated", "Escape", "Poll", "SendData", "EnumFile", "WriteFile",
  "BuildMap", "SetMap", "GetImage"};
#define JC(i) do { uint32_t k_ = LD32(ARG(0) + 4); if (k_ < MAX_DEVS && devs[k_].type == DEV_JOY) jcalls[i]++; } while (0)
EMSCRIPTEN_KEEPALIVE void di_stats(void) {
  fprintf(stderr, "DirectInput joystick calls:");
  for (int i = 0; i < 32; i++) if (jcalls[i]) fprintf(stderr, " %s=%d", jnames[i], jcalls[i]);
  fprintf(stderr, "\n");
  for (int k = 0; k < MAX_DEVS; k++) {
    Dev *d = &devs[k];
    if (!d->used || d->type != DEV_JOY) continue;
    fprintf(stderr, "  pad%d acq=%d size=%u map:", d->pad, d->acquired, d->data_size);
    for (int m = 0; m < d->nmapped; m++) fprintf(stderr, " %c%d@%u", "ABP"[d->map[m].kind], d->map[m].index, d->map[m].ofs);
    fprintf(stderr, " range=%d..%d dz=%d\n", d->range_min[0], d->range_max[0], d->deadzone[0]);
  }
}

static void dev_QueryInterface(void) { ST32(ARG(2), ARG(0)); RET(3, DI_OK); }
static void dev_AddRef(void) { RET(1, 2); }
static void dev_Release(void) {
  Dev *d = dev_of(ARG(0));
  d->used = 0;
  RET(1, 0);
}

static void dev_GetCapabilities(void) { JC(3);
  Dev *d = dev_of(ARG(0));
  uint32_t p = ARG(1), sz = LD32(p);
  memset(GPTR(p + 4), 0, (sz ? sz : 44) - 4);
  ST32(p + 4, 1); /* DIDC_ATTACHED */
  if (d->type == DEV_JOY) { ST32(p + 4, 1 | 0x200); ST32(p + 8, 0x00010215); ST32(p + 12, NJOY_AXES); ST32(p + 16, NJOY_BUTTONS); ST32(p + 20, 1); }
  else if (d->type == DEV_MOUSE) { ST32(p + 8, 0x0112); ST32(p + 12, 3); ST32(p + 16, 4); }
  else { ST32(p + 8, 0x0413); ST32(p + 16, 128); }
  RET(2, DI_OK);
}

static uint32_t obj_type(const DevObj *o, Dev *d, int idx_in_kind) {
  if (o->kind == OBJ_AXIS) return (d->type == DEV_MOUSE ? 1u : 2u) | ((uint32_t)idx_in_kind << 8);
  if (o->kind == OBJ_BUTTON) return 4u | ((uint32_t)o->index << 8);
  return 0x10u | ((uint32_t)o->index << 8);
}

static void dev_EnumObjects(void) { JC(4); /* (this, cb, ref, flags) */
  Dev *d = dev_of(ARG(0));
  uint32_t cb = ARG(1), ref = ARG(2), flags = ARG(3);
  int n;
  const DevObj *objs = dev_objs(d, &n);
  uint32_t p = sys_alloc(316);
  if (d->type == DEV_KEYBOARD) RET(4, DI_OK);
  for (int k = 0; k < n; k++) {
    const DevObj *o = &objs[k];
    uint32_t t = obj_type(o, d, o->index);
    if (flags && flags != 0xFFFFFFFFu) {
      if (o->kind == OBJ_AXIS && !(flags & 3)) continue;
      if (o->kind == OBJ_BUTTON && !(flags & 0xC)) continue;
      if (o->kind == OBJ_POV && !(flags & 0x10)) continue;
    }
    memset(GPTR(p), 0, 316);
    ST32(p, 316);
    write_guid(p + 4, o->guid1, 0xC9F3, 0x11CF, SYS_D4);
    int ofs = find_mapped(d, o->kind, o->index);
    ST32(p + 20, ofs >= 0 ? (uint32_t)ofs : 0);
    ST32(p + 24, t);
    strcpy((char *)GPTR(p + 32), o->name);
    uint32_t a[2] = {p, ref};
    if (!guest_call(cb, 2, a, 0)) break;
  }
  RET(4, DI_OK);
}

static int prop_axis(Dev *d, uint32_t hdr) {
  uint32_t obj = LD32(hdr + 8), how = LD32(hdr + 12);
  if (how == 0) return -1; /* device */
  int n;
  const DevObj *objs = dev_objs(d, &n);
  for (int k = 0; k < n; k++) {
    if (objs[k].kind != OBJ_AXIS) continue;
    if (how == 1 && find_mapped(d, OBJ_AXIS, objs[k].index) == (int)obj) return objs[k].index;
    if (how == 2 && ((obj >> 8) & 0xffff) == (uint32_t)objs[k].index && (obj & 3)) return objs[k].index;
  }
  return -2;
}

static void dev_GetProperty(void) { JC(5); /* (this, prop, hdr) */
  Dev *d = dev_of(ARG(0));
  uint32_t prop = ARG(1), hdr = ARG(2);
  int ax = prop_axis(d, hdr);
  if (ax < -1) RET(3, DIERR_INVALIDPARAM);
  int a = ax < 0 ? 0 : ax;
  switch (prop) {
    case 1: ST32(hdr + 16, d->buffer_size); RET(3, DI_OK);         /* BUFFERSIZE */
    case 2: ST32(hdr + 16, d->type == DEV_MOUSE); RET(3, DI_OK);   /* AXISMODE */
    case 4: ST32(hdr + 16, (uint32_t)d->range_min[a]); ST32(hdr + 20, (uint32_t)d->range_max[a]); RET(3, DI_OK);
    case 5: ST32(hdr + 16, (uint32_t)d->deadzone[a]); RET(3, DI_OK);
    case 6: ST32(hdr + 16, 10000); RET(3, DI_OK);                  /* SATURATION */
    case 7: ST32(hdr + 16, 10000); RET(3, DI_OK);                  /* FFGAIN */
    case 0x16: ST32(hdr + 16, 0); RET(3, DI_OK);                   /* JOYSTICKID */
  }
  RET(3, DIERR_UNSUPPORTED);
}

static void dev_SetProperty(void) { JC(6);
  Dev *d = dev_of(ARG(0));
  uint32_t prop = ARG(1), hdr = ARG(2);
  int ax = prop_axis(d, hdr);
  if (ax < -1) RET(3, DIERR_INVALIDPARAM);
  int lo = ax < 0 ? 0 : ax, hi = ax < 0 ? 7 : ax;
  for (int a = lo; a <= hi; a++) {
    if (prop == 4) { d->range_min[a] = (int32_t)LD32(hdr + 16); d->range_max[a] = (int32_t)LD32(hdr + 20); }
    if (prop == 5) d->deadzone[a] = (int32_t)LD32(hdr + 16);
  }
  if (prop == 1) d->buffer_size = LD32(hdr + 16);
  RET(3, DI_OK);
}

static void dev_Acquire(void) { JC(7); Dev *d = dev_of(ARG(0)); int was = d->acquired; d->acquired = 1; RET(1, was ? DI_NOEFFECT : DI_OK); }
static void dev_Unacquire(void) { Dev *d = dev_of(ARG(0)); d->acquired = 0; d->bhead = d->btail = 0; RET(1, DI_OK); }

static int32_t scale_axis(Dev *d, int a, double v) { /* v in [-1,1] */
  double dz = d->deadzone[a] / 10000.0;
  double m = fabs(v);
  if (m < dz) v = 0;
  else if (dz > 0) v = (v < 0 ? -1 : 1) * (m - dz) / (1 - dz);
  double lo = d->range_min[a], hi = d->range_max[a];
  double r = lo + (v + 1.0) * 0.5 * (hi - lo);
  return (int32_t)lrint(r);
}

static void joy_read(Dev *d, int32_t *axes, uint32_t *buttons, int32_t *pov) {
  int32_t raw[6] = {0};
  uint32_t b = 0;
  int32_t p = -1;
  if (!js_gamepad_state(d->pad, raw, &b, &p)) { memset(raw, 0, sizeof raw); b = 0; p = -1; }
  /* raw axes are in [-32768, 32767]: LX LY LT-RT RX RY */
  for (int a = 0; a < NJOY_AXES; a++) axes[a] = scale_axis(d, a, raw[a] / 32767.0 > 1 ? 1 : raw[a] / 32767.0);
  *buttons = b;
  *pov = p;
}

static void joy_buffer_changes(Dev *d) {
  int32_t axes[8];
  uint32_t b;
  int32_t p;
  joy_read(d, axes, &b, &p);
  for (int a = 0; a < NJOY_AXES; a++)
    if (axes[a] != d->last_axes[a]) { int o = find_mapped(d, OBJ_AXIS, a); if (o >= 0) buf_push(d, o, (uint32_t)axes[a]); d->last_axes[a] = axes[a]; }
  for (int k = 0; k < NJOY_BUTTONS; k++)
    if (((b ^ d->last_buttons) >> k) & 1) { int o = find_mapped(d, OBJ_BUTTON, k); if (o >= 0) buf_push(d, o, (b >> k) & 1 ? 0x80 : 0); }
  d->last_buttons = b;
  if (p != d->last_pov) { int o = find_mapped(d, OBJ_POV, 0); if (o >= 0) buf_push(d, o, (uint32_t)p); d->last_pov = p; }
}

static void dev_GetDeviceState(void) { JC(9); /* (this, cb, data) */
  Dev *d = dev_of(ARG(0));
  uint32_t cb = ARG(1), data = ARG(2);
  pump_host_events();
  if (!d->acquired) RET(3, DIERR_NOTACQUIRED);
  memset(GPTR(data), 0, cb);
  if (d->type == DEV_KEYBOARD) {
    if (!d->nmapped) memcpy(GPTR(data), g_keys, cb < 256 ? cb : 256);
    else for (int k = 0; k < d->nmapped; k++) if (d->map[k].ofs < cb) ST8(data + d->map[k].ofs, g_keys[d->map[k].index & 0xff]);
    RET(3, DI_OK);
  }
  if (d->type == DEV_MOUSE) {
    for (int k = 0; k < d->nmapped; k++) {
      Mapped *m = &d->map[k];
      if (m->ofs + 4 > cb && m->kind == OBJ_AXIS) continue;
      if (m->kind == OBJ_AXIS) ST32(data + m->ofs, (uint32_t)(m->index == 0 ? g_mouse_dx : m->index == 1 ? g_mouse_dy : g_mouse_dz));
      else if (m->ofs < cb) ST8(data + m->ofs, g_mouse_buttons[m->index & 3]);
    }
    g_mouse_dx = g_mouse_dy = g_mouse_dz = 0;
    RET(3, DI_OK);
  }
  int32_t axes[8];
  uint32_t b;
  int32_t p;
  joy_read(d, axes, &b, &p);
  for (int k = 0; k < d->nmapped; k++) {
    Mapped *m = &d->map[k];
    if (m->kind == OBJ_AXIS && m->ofs + 4 <= cb) ST32(data + m->ofs, (uint32_t)axes[m->index]);
    else if (m->kind == OBJ_BUTTON && m->ofs < cb) ST8(data + m->ofs, (b >> m->index) & 1 ? 0x80 : 0);
    else if (m->kind == OBJ_POV && m->ofs + 4 <= cb) ST32(data + m->ofs, (uint32_t)p);
  }
  RET(3, DI_OK);
}

static void dev_GetDeviceData(void) { JC(10); /* (this, cbObj, rgdod, inout, flags) */
  Dev *d = dev_of(ARG(0));
  uint32_t cbo = ARG(1), out = ARG(2), inout = ARG(3), flags = ARG(4);
  pump_host_events();
  if (!d->acquired) { ST32(inout, 0); RET(5, DIERR_NOTACQUIRED); }
  if (d->type == DEV_JOY) joy_buffer_changes(d);
  uint32_t want = LD32(inout), n = 0;
  int h = d->bhead;
  while (h != d->btail && n < want) {
    if (out) {
      uint32_t p = out + n * cbo;
      memset(GPTR(p), 0, cbo);
      ST32(p, d->buf[h].ofs); ST32(p + 4, d->buf[h].data); ST32(p + 8, d->buf[h].time); ST32(p + 12, d->buf[h].seq);
    }
    n++;
    h = (h + 1) % BUFSZ;
  }
  if (!(flags & 1)) d->bhead = h; /* DIGDD_PEEK */
  ST32(inout, n);
  RET(5, DI_OK);
}

static void dev_SetDataFormat(void) { /* (this, df) */
  Dev *d = dev_of(ARG(0));
  uint32_t df = ARG(1);
  uint32_t objsz = LD32(df + 4), dsize = LD32(df + 12), nobj = LD32(df + 16), rg = LD32(df + 20);
  d->data_size = dsize;
  d->nmapped = 0;
  int n;
  const DevObj *objs = dev_objs(d, &n);
  uint8_t used[32] = {0};
  for (uint32_t k = 0; k < nobj && d->nmapped < 300; k++) {
    uint32_t o = rg + k * objsz;
    uint32_t pg = LD32(o), ofs = LD32(o + 4), type = LD32(o + 8);
    uint32_t inst = (type >> 8) & 0xffff;
    int any = inst == 0xffff;
    if (d->type == DEV_KEYBOARD) {
      if (any) continue;
      d->map[d->nmapped].ofs = ofs;
      d->map[d->nmapped].kind = OBJ_BUTTON;
      d->map[d->nmapped].index = (int)inst;
      d->nmapped++;
      continue;
    }
    for (int j = 0; j < n; j++) {
      if (used[j]) continue;
      const DevObj *ob = &objs[j];
      if (pg) { if (!guid_is(pg, ob->guid1)) continue; }
      else {
        if ((type & 3) && ob->kind != OBJ_AXIS) continue;
        if ((type & 0xC) && ob->kind != OBJ_BUTTON) continue;
        if ((type & 0x10) && ob->kind != OBJ_POV) continue;
      }
      if (!any && ob->kind == OBJ_BUTTON && (int)inst != ob->index) continue;
      if (!any && ob->kind == OBJ_POV && (int)inst != ob->index) continue;
      used[j] = 1;
      d->map[d->nmapped].ofs = ofs;
      d->map[d->nmapped].kind = ob->kind;
      d->map[d->nmapped].index = ob->index;
      d->nmapped++;
      break;
    }
  }
  LOG("DirectInput: SetDataFormat type=%d size=%u objs=%u mapped=%d\n", d->type, dsize, nobj, d->nmapped);
  RET(2, DI_OK);
}

static void dev_SetEventNotification(void) { RET(2, DI_OK); }
static void dev_SetCooperativeLevel(void) { JC(13); RET(3, DI_OK); }
static void dev_GetObjectInfo(void) { JC(14); RET(4, DIERR_UNSUPPORTED); }
static void dev_GetDeviceInfo(void) { JC(15); Dev *d = dev_of(ARG(0)); fill_devinst(ARG(1), d->type, d->pad); RET(2, DI_OK); }
static void dev_RunControlPanel(void) { RET(3, DI_OK); }
static void dev_Initialize(void) { RET(4, DI_OK); }
static void dev_CreateEffect(void) { if (ARG(3)) ST32(ARG(3), 0); RET(5, DIERR_UNSUPPORTED); }
static void dev_EnumEffects(void) { RET(4, DI_OK); }
static void dev_GetEffectInfo(void) { RET(3, DIERR_UNSUPPORTED); }
static void dev_GetForceFeedbackState(void) { RET(2, DIERR_UNSUPPORTED); }
static void dev_SendForceFeedbackCommand(void) { RET(2, DIERR_UNSUPPORTED); }
static void dev_EnumCreatedEffectObjects(void) { RET(4, DI_OK); }
static void dev_Escape(void) { RET(2, DIERR_UNSUPPORTED); }
static void dev_Poll(void) { /* the game acquires a joystick only after Poll reports it is not acquired */
  JC(25);
  Dev *d = dev_of(ARG(0));
  RET(1, d->acquired ? DI_NOEFFECT : DIERR_NOTACQUIRED);
}
static void dev_SendDeviceData(void) { RET(5, DIERR_UNSUPPORTED); }
static void dev_EnumEffectsInFile(void) { RET(5, DIERR_UNSUPPORTED); }
static void dev_WriteEffectToFile(void) { RET(5, DIERR_UNSUPPORTED); }
static void dev_BuildActionMap(void) { RET(4, DIERR_UNSUPPORTED); }
static void dev_SetActionMap(void) { RET(4, DIERR_UNSUPPORTED); }
static void dev_GetImageInfo(void) { RET(2, DIERR_UNSUPPORTED); }

static uint32_t make_vtbl(gfn *fns, const char **names, int n) {
  uint32_t v = sys_alloc(4 * (uint32_t)n);
  for (int k = 0; k < n; k++) ST32(v + 4 * k, hle_thunk(fns[k], names[k]));
  return v;
}

#define V(x) x
void dinput_init(void) {
  gfn di_fns[] = {di_QueryInterface, di_AddRef, di_Release, di_CreateDevice, di_EnumDevices, di_GetDeviceStatus,
                  di_RunControlPanel, di_Initialize, di_FindDevice, di_EnumDevicesBySemantics, di_ConfigureDevices};
  const char *di_names[] = {"IDirectInput8::QueryInterface", "IDirectInput8::AddRef", "IDirectInput8::Release",
                            "IDirectInput8::CreateDevice", "IDirectInput8::EnumDevices", "IDirectInput8::GetDeviceStatus",
                            "IDirectInput8::RunControlPanel", "IDirectInput8::Initialize", "IDirectInput8::FindDevice",
                            "IDirectInput8::EnumDevicesBySemantics", "IDirectInput8::ConfigureDevices"};
  gfn dev_fns[] = {dev_QueryInterface, dev_AddRef, dev_Release, dev_GetCapabilities, dev_EnumObjects, dev_GetProperty,
                   dev_SetProperty, dev_Acquire, dev_Unacquire, dev_GetDeviceState, dev_GetDeviceData, dev_SetDataFormat,
                   dev_SetEventNotification, dev_SetCooperativeLevel, dev_GetObjectInfo, dev_GetDeviceInfo,
                   dev_RunControlPanel, dev_Initialize, dev_CreateEffect, dev_EnumEffects, dev_GetEffectInfo,
                   dev_GetForceFeedbackState, dev_SendForceFeedbackCommand, dev_EnumCreatedEffectObjects, dev_Escape,
                   dev_Poll, dev_SendDeviceData, dev_EnumEffectsInFile, dev_WriteEffectToFile, dev_BuildActionMap,
                   dev_SetActionMap, dev_GetImageInfo};
  const char *dev_names[] = {"IDirectInputDevice8::QueryInterface", "IDirectInputDevice8::AddRef", "IDirectInputDevice8::Release",
                             "IDirectInputDevice8::GetCapabilities", "IDirectInputDevice8::EnumObjects", "IDirectInputDevice8::GetProperty",
                             "IDirectInputDevice8::SetProperty", "IDirectInputDevice8::Acquire", "IDirectInputDevice8::Unacquire",
                             "IDirectInputDevice8::GetDeviceState", "IDirectInputDevice8::GetDeviceData", "IDirectInputDevice8::SetDataFormat",
                             "IDirectInputDevice8::SetEventNotification", "IDirectInputDevice8::SetCooperativeLevel",
                             "IDirectInputDevice8::GetObjectInfo", "IDirectInputDevice8::GetDeviceInfo", "IDirectInputDevice8::RunControlPanel",
                             "IDirectInputDevice8::Initialize", "IDirectInputDevice8::CreateEffect", "IDirectInputDevice8::EnumEffects",
                             "IDirectInputDevice8::GetEffectInfo", "IDirectInputDevice8::GetForceFeedbackState",
                             "IDirectInputDevice8::SendForceFeedbackCommand", "IDirectInputDevice8::EnumCreatedEffectObjects",
                             "IDirectInputDevice8::Escape", "IDirectInputDevice8::Poll", "IDirectInputDevice8::SendDeviceData",
                             "IDirectInputDevice8::EnumEffectsInFile", "IDirectInputDevice8::WriteEffectToFile",
                             "IDirectInputDevice8::BuildActionMap", "IDirectInputDevice8::SetActionMap", "IDirectInputDevice8::GetImageInfo"};
  vtbl_di = make_vtbl(di_fns, di_names, 11);
  vtbl_dev = make_vtbl(dev_fns, dev_names, 32);
  di_obj = sys_alloc(8);
  ST32(di_obj, vtbl_di);
}
