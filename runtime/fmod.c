/* FMOD 3.75 (fmod.dll) emulation on top of WebAudio (see the audio part of lib.js).
 * The game loads raw 16-bit PCM samples from memory and streams .ogg music files. */
#include "hle.h"
#include "js.h"
#include "win32.h"

/* JS audio backend */
void js_audio_init(void);
int js_snd_create_pcm(const void *data, int bytes, int bits, int channels, int is_signed, int rate);
int js_snd_load_file(const char *path);   /* async: fetch + decode; -1 on failure */
void js_snd_free(int buf);
void js_ch_play(int ch, int buf, int loop, int paused, double rate_scale, double gain, double pan);
void js_ch_stop(int ch);
int js_ch_is_playing(int ch);
void js_ch_set_paused(int ch, int paused);
void js_ch_set_gain(int ch, double gain);
void js_ch_set_pan(int ch, double pan);
void js_ch_set_rate(int ch, double rate_scale);
void js_ch_set_position(int ch, double seconds);
int js_snd_rate(int buf);

#define FSOUND_FREE -1
#define FSOUND_ALL -3
#define FSOUND_LOOP_OFF 0x1
#define FSOUND_LOOP_NORMAL 0x2
#define FSOUND_LOOP_BIDI 0x4
#define FSOUND_8BITS 0x8
#define FSOUND_16BITS 0x10
#define FSOUND_MONO 0x20
#define FSOUND_STEREO 0x40
#define FSOUND_UNSIGNED 0x80
#define FSOUND_SIGNED 0x100
#define FSOUND_LOADMEMORY 0x8000
#define FSOUND_LOADRAW 0x10000

enum { FMOD_ERR_NONE, FMOD_ERR_BUSY, FMOD_ERR_UNINITIALIZED, FMOD_ERR_INIT, FMOD_ERR_ALLOCATED, FMOD_ERR_PLAY,
       FMOD_ERR_OUTPUT_FORMAT, FMOD_ERR_COOPERATIVELEVEL, FMOD_ERR_CREATEBUFFER, FMOD_ERR_FILE_NOTFOUND,
       FMOD_ERR_FILE_FORMAT, FMOD_ERR_FILE_BAD, FMOD_ERR_MEMORY, FMOD_ERR_VERSION, FMOD_ERR_INVALID_PARAM,
       FMOD_ERR_NO_EAX, FMOD_ERR_CHANNEL_ALLOC };

#define MAX_SAMPLES 1024
#define MAX_CHANNELS 64
typedef struct { int used, buf, rate, mode; uint32_t guest; } Sample;
typedef struct { int used, buf, rate, mode, chan; uint32_t guest; } Stream;
typedef struct {
  int active, serial, paused, vol, pan, freq, is_stream, buf, base_rate;
  double start_time;
} Channel;

static Sample samples[MAX_SAMPLES];
static Stream streams[64];
static Channel chans[MAX_CHANNELS];
static int nchans = 32;
static int last_err;
static int sfx_master = 255;
static int inited;

static uint32_t ch_handle(int k) { return (uint32_t)k | ((uint32_t)chans[k].serial << 12); }

static Channel *ch_get(uint32_t h, int *idx) {
  int k = (int)(h & 0xfff);
  if ((int32_t)h < 0 || k >= nchans) return NULL;
  if (((h >> 12) & 0xfffff) != (uint32_t)chans[k].serial) return NULL;
  if (idx) *idx = k;
  return &chans[k];
}

static double gain_of(Channel *c) {
  double g = c->vol / 255.0;
  if (!c->is_stream) g *= sfx_master / 255.0;
  return g;
}
static double pan_of(Channel *c) { return c->pan < 0 ? 0.0 : (c->pan - 128) / 128.0; }
static double rate_of(Channel *c) { return c->base_rate ? (double)c->freq / c->base_rate : 1.0; }

static int alloc_channel(int want) {
  if (want >= 0) {
    if (want >= nchans) return -1;
    js_ch_stop(want);
    return want;
  }
  for (int k = 0; k < nchans; k++)
    if (!chans[k].active || !js_ch_is_playing(k)) {
      if (chans[k].active && chans[k].paused) continue; /* paused channels stay reserved */
      return k;
    }
  /* steal the oldest non-stream channel */
  int best = -1;
  for (int k = 0; k < nchans; k++)
    if (!chans[k].is_stream && (best < 0 || chans[k].start_time < chans[best].start_time)) best = k;
  if (best >= 0) js_ch_stop(best);
  return best;
}

static uint32_t start_channel(int want, int buf, int rate, int mode, int paused, int is_stream) {
  int k = alloc_channel(want);
  if (k < 0) { last_err = FMOD_ERR_CHANNEL_ALLOC; return 0xFFFFFFFFu; }
  Channel *c = &chans[k];
  c->active = 1;
  c->serial = (c->serial + 1) & 0xfffff;
  c->paused = paused;
  c->vol = 255;
  c->pan = 128;
  c->freq = rate;
  c->base_rate = rate;
  c->is_stream = is_stream;
  c->buf = buf;
  c->start_time = host_now_us();
  int loop = (mode & (FSOUND_LOOP_NORMAL | FSOUND_LOOP_BIDI)) != 0;
  js_ch_play(k, buf, loop, paused, 1.0, gain_of(c), pan_of(c));
  return ch_handle(k);
}

/* ------------------------------------------------------------------ API */
HLE(fmod, _FSOUND_GetVersion_0) { FPUSH(3.75); RETV(0); }
HLE(fmod, _FSOUND_GetError_0) { RET(0, (uint32_t)last_err); }
HLE(fmod, _FSOUND_SetOutput_4) { RET(1, 1); }
HLE(fmod, _FSOUND_SetSpeakerMode_4) { RETV(1); }

HLE(fmod, _FSOUND_Init_12) { /* (mixrate, maxchannels, flags) */
  int n = (int)ARG(1);
  nchans = n < 1 ? 32 : n > MAX_CHANNELS ? MAX_CHANNELS : n;
  js_audio_init();
  inited = 1;
  RET(3, 1);
}

HLE(fmod, _FSOUND_Close_0) {
  for (int k = 0; k < nchans; k++) js_ch_stop(k);
  inited = 0;
  RETV(0);
}

HLE(fmod, _FSOUND_Update_0) { RETV(0); }

HLE(fmod, _FSOUND_SetSFXMasterVolume_4) {
  sfx_master = (int)ARG(0) & 0xff;
  for (int k = 0; k < nchans; k++)
    if (chans[k].active && !chans[k].is_stream) js_ch_set_gain(k, gain_of(&chans[k]));
  RETV(1);
}

HLE(fmod, _FSOUND_Sample_Load_20) { /* (index, name_or_data, mode, offset, length) */
  int32_t index = (int32_t)ARG(0);
  uint32_t src = ARG(1), mode = ARG(2), off = ARG(3), len = ARG(4);
  int k = -1;
  if (index >= 0 && index < MAX_SAMPLES) k = index;
  else for (int j = 0; j < MAX_SAMPLES; j++) if (!samples[j].used) { k = j; break; }
  if (k < 0) { last_err = FMOD_ERR_MEMORY; RET(5, 0); }
  int buf;
  int rate = 44100;
  if (mode & FSOUND_LOADMEMORY) {
    if (!(mode & FSOUND_LOADRAW)) {
      LOG("FSOUND_Sample_Load: memory, non-raw mode %x unsupported\n", mode);
      last_err = FMOD_ERR_FILE_FORMAT;
      RET(5, 0);
    }
    int bits = (mode & FSOUND_8BITS) ? 8 : 16;
    int chn = (mode & FSOUND_STEREO) ? 2 : 1;
    int sgn = (mode & FSOUND_UNSIGNED) ? 0 : (mode & FSOUND_SIGNED) ? 1 : bits == 16;
    buf = js_snd_create_pcm(GPTR(src + off), (int)len, bits, chn, sgn, rate);
  } else {
    char path[512];
    path_to_host((char *)GPTR(src), path, sizeof path);
    buf = js_snd_load_file(path);
    note_yield();
    if (buf >= 0) rate = js_snd_rate(buf);
  }
  if (buf < 0) { last_err = FMOD_ERR_FILE_BAD; RET(5, 0); }
  Sample *s = &samples[k];
  if (s->used) js_snd_free(s->buf);
  s->used = 1;
  s->buf = buf;
  s->rate = rate;
  s->mode = (int)mode;
  if (!s->guest) s->guest = sys_alloc(16);
  ST32(s->guest, (uint32_t)k);
  RET(5, s->guest);
}

static Sample *sample_of(uint32_t p) {
  if (!p) return NULL;
  uint32_t k = LD32(p);
  if (k >= MAX_SAMPLES || !samples[k].used || samples[k].guest != p) return NULL;
  return &samples[k];
}

HLE(fmod, _FSOUND_Sample_Free_4) {
  Sample *s = sample_of(ARG(0));
  if (s) {
    for (int k = 0; k < nchans; k++)
      if (chans[k].active && !chans[k].is_stream && chans[k].buf == s->buf) { js_ch_stop(k); chans[k].active = 0; }
    js_snd_free(s->buf);
    s->used = 0;
  }
  RETV(1);
}

HLE(fmod, _FSOUND_PlaySoundEx_16) { /* (channel, sample, dsp, paused) */
  Sample *s = sample_of(ARG(1));
  if (!s) { last_err = FMOD_ERR_INVALID_PARAM; RET(4, 0xFFFFFFFFu); }
  RET(4, start_channel((int32_t)ARG(0), s->buf, s->rate, s->mode, ARG(3) & 0xff, 0));
}

HLE(fmod, _FSOUND_Stream_Open_16) { /* (name, mode, offset, length) */
  char path[512];
  path_to_host(ARGP(0), path, sizeof path);
  int k = -1;
  for (int j = 0; j < 64; j++) if (!streams[j].used) { k = j; break; }
  if (k < 0) { last_err = FMOD_ERR_MEMORY; RET(4, 0); }
  int buf = js_snd_load_file(path);
  note_yield();
  LOG("FSOUND_Stream_Open(%s) -> %d\n", path, buf);
  if (buf < 0) { last_err = FMOD_ERR_FILE_NOTFOUND; RET(4, 0); }
  Stream *s = &streams[k];
  s->used = 1;
  s->buf = buf;
  s->rate = js_snd_rate(buf);
  s->mode = (int)ARG(1);
  s->chan = -1;
  if (!s->guest) s->guest = sys_alloc(16);
  ST32(s->guest, (uint32_t)k);
  RET(4, s->guest);
}

static Stream *stream_of(uint32_t p) {
  if (!p) return NULL;
  uint32_t k = LD32(p);
  if (k >= 64 || !streams[k].used || streams[k].guest != p) return NULL;
  return &streams[k];
}

HLE(fmod, _FSOUND_Stream_PlayEx_16) { /* (channel, stream, dsp, paused) */
  Stream *s = stream_of(ARG(1));
  if (!s) { last_err = FMOD_ERR_INVALID_PARAM; RET(4, 0xFFFFFFFFu); }
  uint32_t h = start_channel((int32_t)ARG(0), s->buf, s->rate, s->mode, ARG(3) & 0xff, 1);
  s->chan = (int)(h & 0xfff);
  RET(4, h);
}

HLE(fmod, _FSOUND_Stream_Close_4) {
  Stream *s = stream_of(ARG(0));
  if (!s) RET(1, 0);
  for (int k = 0; k < nchans; k++)
    if (chans[k].active && chans[k].is_stream && chans[k].buf == s->buf) { js_ch_stop(k); chans[k].active = 0; }
  js_snd_free(s->buf);
  s->used = 0;
  RET(1, 1);
}

#define FOR_CHANNELS(h, body)                                              \
  do {                                                                     \
    if ((int32_t)(h) == FSOUND_ALL) {                                      \
      for (int _k = 0; _k < nchans; _k++)                                  \
        if (chans[_k].active) { int k = _k; Channel *c = &chans[k]; body; } \
    } else {                                                               \
      int k;                                                               \
      Channel *c = ch_get(h, &k);                                          \
      if (!c) { last_err = FMOD_ERR_INVALID_PARAM; RET(2, 0); }            \
      body;                                                                \
    }                                                                      \
  } while (0)

HLE(fmod, _FSOUND_StopSound_4) {
  int32_t h = (int32_t)ARG(0);
  if (h == FSOUND_ALL) {
    for (int k = 0; k < nchans; k++) if (chans[k].active) { js_ch_stop(k); chans[k].active = 0; }
    RET(1, 1);
  }
  int k;
  Channel *c = ch_get((uint32_t)h, &k);
  if (!c) RET(1, 0);
  js_ch_stop(k);
  c->active = 0;
  RET(1, 1);
}

HLE(fmod, _FSOUND_IsPlaying_4) {
  int k;
  Channel *c = ch_get(ARG(0), &k);
  if (!c || !c->active) RET(1, 0);
  RET(1, c->paused || js_ch_is_playing(k) ? 1 : 0);
}

HLE(fmod, _FSOUND_GetPaused_4) {
  Channel *c = ch_get(ARG(0), NULL);
  RET(1, c && c->paused ? 1 : 0);
}

HLE(fmod, _FSOUND_SetPaused_8) {
  int p = ARG(1) & 0xff;
  FOR_CHANNELS(ARG(0), { if (c->paused != p) { c->paused = p; js_ch_set_paused(k, p); } });
  RET(2, 1);
}

HLE(fmod, _FSOUND_SetVolume_8) {
  int v = (int)ARG(1);
  if (v < 0) v = 0;
  if (v > 255) v = 255;
  FOR_CHANNELS(ARG(0), { c->vol = v; js_ch_set_gain(k, gain_of(c)); });
  RET(2, 1);
}

HLE(fmod, _FSOUND_SetPan_8) {
  int p = (int)ARG(1);
  if (p != -1) { if (p < 0) p = 0; if (p > 255) p = 255; }
  FOR_CHANNELS(ARG(0), { c->pan = p; js_ch_set_pan(k, pan_of(c)); });
  RET(2, 1);
}

HLE(fmod, _FSOUND_SetFrequency_8) {
  int f = (int)ARG(1);
  if (f < 100) f = 100;
  if (f > 705600) f = 705600;
  FOR_CHANNELS(ARG(0), { c->freq = f; js_ch_set_rate(k, rate_of(c)); });
  RET(2, 1);
}

HLE(fmod, _FSOUND_SetCurrentPosition_8) { /* (channel, offset in samples) */
  uint32_t off = ARG(1);
  FOR_CHANNELS(ARG(0), { js_ch_set_position(k, c->base_rate ? (double)off / c->base_rate : 0.0); });
  RET(2, 1);
}
