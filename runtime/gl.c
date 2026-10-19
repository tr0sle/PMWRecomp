/* OpenGL 1.1 (+ARB_multitexture) fixed-function emulation on WebGL2.
 * Fixed-function state is tracked here and turned into generated GLSL ES 3.0
 * programs (cached by a state key). Client-side vertex arrays live in guest
 * memory, which is wasm memory, so they are uploaded straight from it. */
#include "hle.h"
#include "js.h"
#include "win32.h"
#include <GLES3/gl3.h>
#include <emscripten/html5.h>
#include <stdarg.h>

/* ------------------------------------------------------------------ GL 1.1 enums not in GLES3 */
#define GL_QUADS 0x0007
#define GL_QUAD_STRIP 0x0008
#define GL_POLYGON 0x0009
#define GL_MODELVIEW 0x1700
#define GL_PROJECTION 0x1701
#define GL_TEXTURE 0x1702
#define GL_MODELVIEW_MATRIX 0x0BA6
#define GL_PROJECTION_MATRIX 0x0BA7
#define GL_TEXTURE_MATRIX 0x0BA8
#define GL_LIGHTING 0x0B50
#define GL_LIGHT0 0x4000
#define GL_COLOR_MATERIAL 0x0B57
#define GL_NORMALIZE 0x0BA1
#define GL_RESCALE_NORMAL 0x803A
#define GL_ALPHA_TEST 0x0BC0
#define GL_FOG 0x0B60
#define GL_TEXTURE_GEN_S 0x0C60
#define GL_TEXTURE_GEN_T 0x0C61
#define GL_TEXTURE_GEN_R 0x0C62
#define GL_TEXTURE_GEN_Q 0x0C63
#define GL_TEXTURE_GEN_MODE 0x2500
#define GL_EYE_LINEAR 0x2400
#define GL_OBJECT_LINEAR 0x2401
#define GL_SPHERE_MAP 0x2402
#define GL_NORMAL_MAP 0x8511
#define GL_REFLECTION_MAP 0x8512
#define GL_VERTEX_ARRAY 0x8074
#define GL_NORMAL_ARRAY 0x8075
#define GL_COLOR_ARRAY 0x8076
#define GL_TEXTURE_COORD_ARRAY 0x8078
#define GL_FLAT 0x1D00
#define GL_SMOOTH 0x1D01
#define GL_AMBIENT 0x1200
#define GL_DIFFUSE 0x1201
#define GL_SPECULAR 0x1202
#define GL_POSITION 0x1203
#define GL_SPOT_DIRECTION 0x1204
#define GL_SPOT_EXPONENT 0x1205
#define GL_SPOT_CUTOFF 0x1206
#define GL_CONSTANT_ATTENUATION 0x1207
#define GL_LINEAR_ATTENUATION 0x1208
#define GL_QUADRATIC_ATTENUATION 0x1209
#define GL_EMISSION 0x1600
#define GL_AMBIENT_AND_DIFFUSE 0x1602
#define GL_LIGHT_MODEL_LOCAL_VIEWER 0x0B51
#define GL_LIGHT_MODEL_TWO_SIDE 0x0B52
#define GL_LIGHT_MODEL_AMBIENT 0x0B53
#define GL_FOG_DENSITY 0x0B62
#define GL_FOG_START 0x0B63
#define GL_FOG_END 0x0B64
#define GL_FOG_MODE 0x0B65
#define GL_FOG_COLOR 0x0B66
#define GL_EXP 0x0800
#define GL_EXP2 0x0801
#define GL_TEXTURE_ENV 0x2300
#define GL_TEXTURE_ENV_MODE 0x2200
#define GL_TEXTURE_ENV_COLOR 0x2201
#define GL_MODULATE 0x2100
#define GL_DECAL 0x2101
#define GL_ADD 0x0104
#define GL_COMBINE 0x8570
#define GL_COMBINE_RGB 0x8571
#define GL_COMBINE_ALPHA 0x8572
#define GL_RGB_SCALE 0x8573
#define GL_ADD_SIGNED 0x8574
#define GL_INTERPOLATE 0x8575
#define GL_CONSTANT 0x8576
#define GL_PRIMARY_COLOR 0x8577
#define GL_PREVIOUS 0x8578
#define GL_SUBTRACT 0x84E7
#define GL_SOURCE0_RGB 0x8580
#define GL_SOURCE0_ALPHA 0x8588
#define GL_OPERAND0_RGB 0x8590
#define GL_OPERAND0_ALPHA 0x8598
#define GL_ALPHA_SCALE 0x0D1C
#define GL_CLAMP 0x2900
#define GL_BGRA 0x80E1
#define GL_BGR 0x80E0
#define GL_INTENSITY 0x8049
#define GL_DOUBLE 0x140A
#define GL_MAX_TEXTURE_UNITS 0x84E2
#define GL_MAX_TEXTURE_MAX_ANISOTROPY 0x84FF
#define GL_TEXTURE_MAX_ANISOTROPY 0x84FE
#define GL_GENERATE_MIPMAP 0x8191
#define GL_POLYGON_MODE 0x0B40
#define GL_TEXTURE0_ARB 0x84C0
#define GL_UNSIGNED_INT_8_8_8_8_REV 0x8367
#define GL_POINT 0x1B00
#define GL_LINE 0x1B01
#define GL_FILL 0x1B02

/* ------------------------------------------------------------------ math */
typedef struct { float m[16]; } Mat;
static void mat_ident(Mat *a) { memset(a, 0, sizeof *a); a->m[0] = a->m[5] = a->m[10] = a->m[15] = 1; }
static void mat_mul(Mat *r, const Mat *a, const Mat *b) { /* r = a * b (column-major) */
  Mat t;
  for (int c = 0; c < 4; c++)
    for (int rr = 0; rr < 4; rr++) {
      float s = 0;
      for (int k = 0; k < 4; k++) s += a->m[k * 4 + rr] * b->m[c * 4 + k];
      t.m[c * 4 + rr] = s;
    }
  *r = t;
}
static void mat_xform(const Mat *a, const float *v, float *o) {
  for (int r = 0; r < 4; r++) o[r] = a->m[r] * v[0] + a->m[4 + r] * v[1] + a->m[8 + r] * v[2] + a->m[12 + r] * v[3];
}
static int mat_inv3(const Mat *a, float out[9]) { /* inverse-transpose of upper 3x3, column-major out */
  const float *m = a->m;
  float a00 = m[0], a01 = m[4], a02 = m[8], a10 = m[1], a11 = m[5], a12 = m[9], a20 = m[2], a21 = m[6], a22 = m[10];
  float c00 = a11 * a22 - a12 * a21, c01 = a12 * a20 - a10 * a22, c02 = a10 * a21 - a11 * a20;
  float det = a00 * c00 + a01 * c01 + a02 * c02;
  if (fabsf(det) < 1e-20f) { memset(out, 0, 36); out[0] = out[4] = out[8] = 1; return 0; }
  float id = 1.0f / det;
  /* inverse transpose = cofactor matrix / det */
  float r[3][3] = {{c00, c01, c02},
                   {a02 * a21 - a01 * a22, a00 * a22 - a02 * a20, a01 * a20 - a00 * a21},
                   {a01 * a12 - a02 * a11, a02 * a10 - a00 * a12, a00 * a11 - a01 * a10}};
  /* r[i][j] is cofactor C_ij -> (A^-1)^T = C/det; store column-major: out[col*3+row] = C[row][col] */
  for (int i = 0; i < 3; i++)
    for (int j = 0; j < 3; j++) out[j * 3 + i] = r[i][j] * id;
  return 1;
}

/* ------------------------------------------------------------------ state */
#define MAX_UNITS 2
#define MV_DEPTH 32
#define OTHER_DEPTH 8
typedef struct {
  Mat mv[MV_DEPTH], proj[OTHER_DEPTH], tex[MAX_UNITS][OTHER_DEPTH];
  int mv_sp, proj_sp, tex_sp[MAX_UNITS];
  int matrix_mode;
  int active_unit, client_unit;
  /* enables */
  int lighting, light_en[8], color_material, normalize, alpha_test, fog, cull, depth_test, blend, scissor, stencil;
  int tex2d[MAX_UNITS], texgen[MAX_UNITS][4];
  int texgen_mode[MAX_UNITS][4];
  /* lights */
  float lamb[8][4], ldif[8][4], lspec[8][4], lpos[8][4], lspot[8][3], lspot_exp[8], lspot_cut[8], latt[8][3];
  float lm_amb[4];
  int local_viewer, two_side;
  float mamb[4], mdif[4], mspec[4], memi[4], mshin;
  int cm_mode;
  /* fog */
  int fog_mode;
  float fog_density, fog_start, fog_end, fog_color[4];
  /* alpha */
  int alpha_func;
  float alpha_ref;
  int shade_flat;
  /* texenv */
  int env_mode[MAX_UNITS];
  int comb_rgb[MAX_UNITS], comb_a[MAX_UNITS], src_rgb[MAX_UNITS][3], src_a[MAX_UNITS][3], op_rgb[MAX_UNITS][3], op_a[MAX_UNITS][3];
  float rgb_scale[MAX_UNITS], a_scale[MAX_UNITS], env_color[MAX_UNITS][4];
  uint32_t bound[MAX_UNITS];
  /* client arrays */
  struct { int enabled, size, type, stride; uint32_t ptr; } va, na, ca, ta[MAX_UNITS];
  /* current attributes */
  float cur_color[4], cur_normal[3], cur_tc[MAX_UNITS][4];
  /* viewport/scissor in game coordinates */
  int vp[4], sc[4];
  int polygon_mode;
} GLState;
static GLState S;

/* textures */
typedef struct { int used; GLuint id; int fmt_class; int w, h; int min_filter; int has_mips; int auto_mips; } Tex;
#define MAX_TEX 8192
static Tex texs[MAX_TEX];
enum { FMT_RGB = 0, FMT_RGBA = 1, FMT_ALPHA = 2, FMT_LUM = 3, FMT_LUMA = 4, FMT_INT = 5 };

static int game_w = 640, game_h = 480;
static int canvas_w = 640, canvas_h = 480;
static float scale_x = 1, scale_y = 1;
static EMSCRIPTEN_WEBGL_CONTEXT_HANDLE ctx;
static int ctx_ok;
static float aniso_max = 1; /* EXT_texture_filter_anisotropic, when the browser has it */

/* ------------------------------------------------------------------ programs */
typedef struct {
  uint64_t key[2];
  GLuint prog;
  GLint u_mvp, u_mv, u_nm, u_tm[MAX_UNITS], u_tex[MAX_UNITS];
  GLint u_lamb, u_ldif, u_lspec, u_lpos, u_lspot, u_lspotp, u_latt, u_lmamb;
  GLint u_mamb, u_mdif, u_mspec, u_memi, u_mshin;
  GLint u_fogc, u_fogp, u_aref, u_envc;
} Prog;
#define MAX_PROGS 512
static Prog progs[MAX_PROGS];
static int nprogs;

enum { A_POS = 0, A_NRM = 1, A_COL = 2, A_TC0 = 3, A_TC1 = 4 };

static const char *alpha_cmp(int f) {
  switch (f) {
    case GL_NEVER: return "false";
    case GL_LESS: return "a < u_aref";
    case GL_EQUAL: return "a == u_aref";
    case GL_LEQUAL: return "a <= u_aref";
    case GL_GREATER: return "a > u_aref";
    case GL_NOTEQUAL: return "a != u_aref";
    case GL_GEQUAL: return "a >= u_aref";
  }
  return "true";
}

static int tex_fmt(int unit) {
  uint32_t t = S.bound[unit];
  if (t < MAX_TEX && texs[t].used) return texs[t].fmt_class;
  return FMT_RGBA;
}

static int unit_on(int u) {
  uint32_t t = S.bound[u];
  return S.tex2d[u] && t && t < MAX_TEX && texs[t].used;
}

typedef struct { char *p; int n, cap; } SB;
static void sb_add(SB *b, const char *fmt, ...) {
  va_list ap;
  for (;;) {
    va_start(ap, fmt);
    int r = vsnprintf(b->p + b->n, b->cap - b->n, fmt, ap);
    va_end(ap);
    if (r < b->cap - b->n) { b->n += r; return; }
    b->cap = b->cap * 2 + r + 64;
    b->p = realloc(b->p, b->cap);
  }
}

static const char *comb_src(int src, int unit, int alpha) {
  static char buf[8][32];
  static int bi;
  char *o = buf[bi++ & 7];
  switch (src) {
    case GL_TEXTURE: snprintf(o, 32, "t%d", unit); break;
    case GL_CONSTANT: snprintf(o, 32, "u_envc[%d]", unit); break;
    case GL_PRIMARY_COLOR: snprintf(o, 32, "v_col"); break;
    case GL_PREVIOUS: default: snprintf(o, 32, "c"); break;
  }
  if (src >= GL_TEXTURE0_ARB && src < GL_TEXTURE0_ARB + MAX_UNITS) snprintf(o, 32, "t%d", src - GL_TEXTURE0_ARB);
  (void)alpha;
  return o;
}

static void comb_arg(SB *b, const char *name, int src, int op, int unit, int alpha) {
  const char *s = comb_src(src, unit, alpha);
  if (!alpha) {
    switch (op) {
      case GL_SRC_COLOR: sb_add(b, "vec3 %s=%s.rgb;", name, s); break;
      case GL_ONE_MINUS_SRC_COLOR: sb_add(b, "vec3 %s=1.0-%s.rgb;", name, s); break;
      case GL_SRC_ALPHA: sb_add(b, "vec3 %s=vec3(%s.a);", name, s); break;
      default: sb_add(b, "vec3 %s=vec3(1.0-%s.a);", name, s); break;
    }
  } else {
    if (op == GL_ONE_MINUS_SRC_ALPHA) sb_add(b, "float %s=1.0-%s.a;", name, s);
    else sb_add(b, "float %s=%s.a;", name, s);
  }
}

static void comb_fn(SB *b, const char *dst, int fn, const char *a0, const char *a1, const char *a2, int alpha) {
  const char *half = alpha ? "0.5" : "vec3(0.5)";
  switch (fn) {
    case GL_REPLACE: sb_add(b, "%s=%s;", dst, a0); break;
    case GL_MODULATE: sb_add(b, "%s=%s*%s;", dst, a0, a1); break;
    case GL_ADD: sb_add(b, "%s=%s+%s;", dst, a0, a1); break;
    case GL_ADD_SIGNED: sb_add(b, "%s=%s+%s-%s;", dst, a0, a1, half); break;
    case GL_INTERPOLATE: sb_add(b, "%s=%s*%s+%s*(1.0-%s);", dst, a0, a2, a1, a2); break;
    case GL_SUBTRACT: sb_add(b, "%s=%s-%s;", dst, a0, a1); break;
    default: sb_add(b, "%s=%s*%s;", dst, a0, a1); break;
  }
}

static GLuint compile(GLenum type, const char *src) {
  GLuint s = glCreateShader(type);
  glShaderSource(s, 1, &src, NULL);
  glCompileShader(s);
  GLint ok = 0;
  glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
  if (!ok) {
    char log[2048];
    glGetShaderInfoLog(s, sizeof log, NULL, log);
    fprintf(stderr, "shader compile error:\n%s\n----\n%s\n", log, src);
    fatal("GL shader compile failed");
  }
  return s;
}

/* builds the state key; must cover everything the generator reads */
static void make_key(uint64_t key[2]) {
  uint64_t k0 = 0, k1 = 0;
  int lit = S.lighting;
  k0 |= (uint64_t)lit;
  if (lit) {
    for (int i = 0; i < 8; i++) k0 |= (uint64_t)S.light_en[i] << (1 + i);
    k0 |= (uint64_t)S.color_material << 9;
    k0 |= (uint64_t)(S.cm_mode & 7) << 10;            /* low bits distinguish 0x1200..0x1602 */
    k0 |= (uint64_t)(S.cm_mode == GL_AMBIENT_AND_DIFFUSE) << 13;
    k0 |= (uint64_t)S.local_viewer << 14;
    k0 |= (uint64_t)S.normalize << 15;
  }
  k0 |= (uint64_t)S.alpha_test << 16;
  if (S.alpha_test) k0 |= (uint64_t)(S.alpha_func & 7) << 17;
  k0 |= (uint64_t)S.fog << 20;
  if (S.fog) k0 |= (uint64_t)(S.fog_mode == GL_LINEAR ? 0 : S.fog_mode == GL_EXP ? 1 : 2) << 21;
  k0 |= (uint64_t)S.shade_flat << 23;
  for (int u = 0; u < MAX_UNITS; u++) {
    int on = unit_on(u);
    uint64_t uk = (uint64_t)on;
    if (on) {
      int em = S.env_mode[u];
      int ec = em == GL_MODULATE ? 0 : em == GL_REPLACE ? 1 : em == GL_DECAL ? 2 : em == GL_BLEND ? 3 : em == GL_ADD ? 4 : 5;
      uk |= (uint64_t)ec << 1;
      uk |= (uint64_t)tex_fmt(u) << 4;
      for (int c = 0; c < 4; c++) {
        if (!S.texgen[u][c]) continue;
        int m = S.texgen_mode[u][c];
        int mc = m == GL_EYE_LINEAR ? 1 : m == GL_OBJECT_LINEAR ? 2 : m == GL_SPHERE_MAP ? 3 : m == GL_REFLECTION_MAP ? 4 : 5;
        uk |= (uint64_t)mc << (7 + c * 3);
      }
      k0 |= uk << (24 + u * 20);
      if (ec == 5) { /* combine */
        uint64_t ck = 0;
        ck |= (uint64_t)(S.comb_rgb[u] & 0xff);
        ck |= (uint64_t)(S.comb_a[u] & 0xff) << 8;
        for (int j = 0; j < 3; j++) {
          ck |= (uint64_t)(S.src_rgb[u][j] & 0x1f) << (16 + j * 5);
          ck |= (uint64_t)(S.op_rgb[u][j] & 3) << (31 + j * 2);
        }
        k1 |= ck << (u * 0);
        k1 ^= ((uint64_t)S.src_a[u][0] * 31 + S.src_a[u][1] * 17 + S.src_a[u][2] * 7 + S.op_a[u][0] * 3 + S.op_a[u][1] + S.op_a[u][2]) << (40 + u * 10);
        k1 ^= (uint64_t)(S.rgb_scale[u] != 1.0f) << (60 + u);
      }
    }
  }
  key[0] = k0;
  key[1] = k1;
}

static Prog *get_prog(void) {
  uint64_t key[2];
  make_key(key);
  static Prog *last;
  if (last && last->key[0] == key[0] && last->key[1] == key[1]) return last;
  for (int i = 0; i < nprogs; i++)
    if (progs[i].key[0] == key[0] && progs[i].key[1] == key[1]) return last = &progs[i];
  if (nprogs >= MAX_PROGS) fatal("too many GL programs");
  Prog *P = &progs[nprogs++];
  memset(P, 0, sizeof *P);
  P->key[0] = key[0];
  P->key[1] = key[1];

  int units_on[MAX_UNITS];
  int any_texgen_sphere = 0, any_eye = 0;
  for (int u = 0; u < MAX_UNITS; u++) {
    units_on[u] = unit_on(u);
    for (int c = 0; c < 4; c++)
      if (units_on[u] && S.texgen[u][c]) {
        if (S.texgen_mode[u][c] == GL_SPHERE_MAP || S.texgen_mode[u][c] == GL_REFLECTION_MAP || S.texgen_mode[u][c] == GL_NORMAL_MAP) any_texgen_sphere = 1;
        any_eye = 1;
      }
  }
  int lit = S.lighting;
  const char *flat = S.shade_flat ? "flat " : "";

  SB v = {0}, f = {0};
  sb_add(&v, "#version 300 es\nprecision highp float;\n"
             "layout(location=0) in vec4 a_pos;layout(location=1) in vec3 a_nrm;layout(location=2) in vec4 a_col;"
             "layout(location=3) in vec4 a_tc0;layout(location=4) in vec4 a_tc1;\n"
             "uniform mat4 u_mvp,u_mv,u_tm[%d];uniform mat3 u_nm;\n"
             "%sout vec4 v_col;out vec4 v_tc0;out vec4 v_tc1;out float v_fogz;\n",
         MAX_UNITS, flat);
  if (lit)
    sb_add(&v, "uniform vec4 u_lamb[8],u_ldif[8],u_lspec[8],u_lpos[8],u_lspotp[8];uniform vec3 u_lspot[8],u_latt[8];"
               "uniform vec4 u_lmamb,u_mamb,u_mdif,u_mspec,u_memi;uniform float u_mshin;\n");
  sb_add(&v, "void main(){\nvec4 ep=u_mv*a_pos;gl_Position=u_mvp*a_pos;v_fogz=abs(ep.z);\n");
  if (lit || any_texgen_sphere) {
    sb_add(&v, "vec3 n=u_nm*a_nrm;");
    if (S.normalize || any_texgen_sphere) sb_add(&v, "n=normalize(n);");
    sb_add(&v, "\n");
  }
  if (lit) {
    int cm = S.color_material;
    const char *amb = cm && (S.cm_mode == GL_AMBIENT || S.cm_mode == GL_AMBIENT_AND_DIFFUSE) ? "a_col" : "u_mamb";
    const char *dif = cm && (S.cm_mode == GL_DIFFUSE || S.cm_mode == GL_AMBIENT_AND_DIFFUSE) ? "a_col" : "u_mdif";
    const char *spc = cm && S.cm_mode == GL_SPECULAR ? "a_col" : "u_mspec";
    const char *emi = cm && S.cm_mode == GL_EMISSION ? "a_col" : "u_memi";
    sb_add(&v, "vec4 mamb=%s,mdif=%s,mspec=%s,memi=%s;\n", amb, dif, spc, emi);
    sb_add(&v, "vec3 col=memi.rgb+mamb.rgb*u_lmamb.rgb;vec3 epn=ep.xyz/ep.w;\n");
    for (int i = 0; i < 8; i++) {
      if (!S.light_en[i]) continue;
      sb_add(&v,
             "{vec4 lp=u_lpos[%d];vec3 L;float att=1.0;"
             "if(lp.w!=0.0){vec3 d=lp.xyz/lp.w-epn;float dl=length(d);L=d/max(dl,1e-6);"
             "att=1.0/max(u_latt[%d].x+u_latt[%d].y*dl+u_latt[%d].z*dl*dl,1e-6);}else L=normalize(lp.xyz);"
             "if(u_lspotp[%d].y>-1.5){float sd=dot(-L,normalize(u_lspot[%d]));att*=sd>=u_lspotp[%d].y?(u_lspotp[%d].x>0.0?pow(max(sd,1e-6),u_lspotp[%d].x):1.0):0.0;}"
             "float nl=max(dot(n,L),0.0);vec3 c=mamb.rgb*u_lamb[%d].rgb+nl*mdif.rgb*u_ldif[%d].rgb;"
             /* GL defines x^0 = 1 for the specular term; GLSL pow(0,0) is undefined (NaN on some GPUs) */
             "if(nl>0.0){vec3 h=normalize(L+%s);float nh=max(dot(n,h),0.0);float sp=u_mshin>0.0?(nh>0.0?pow(nh,u_mshin):0.0):1.0;"
             "c+=sp*mspec.rgb*u_lspec[%d].rgb;}"
             "col+=att*c;}\n",
             i, i, i, i, i, i, i, i, i, i, i, S.local_viewer ? "normalize(-epn)" : "vec3(0.0,0.0,1.0)", i);
    }
    sb_add(&v, "v_col=vec4(clamp(col,0.0,1.0),clamp(mdif.a,0.0,1.0));\n");
  } else {
    sb_add(&v, "v_col=a_col;\n");
  }
  for (int u = 0; u < MAX_UNITS; u++) {
    if (!units_on[u]) { sb_add(&v, "v_tc%d=vec4(0.0,0.0,0.0,1.0);\n", u); continue; }
    sb_add(&v, "{vec4 tc=a_tc%d;", u);
    int needs_r = 0;
    for (int c = 0; c < 4; c++)
      if (S.texgen[u][c] && (S.texgen_mode[u][c] == GL_SPHERE_MAP || S.texgen_mode[u][c] == GL_REFLECTION_MAP)) needs_r = 1;
    if (needs_r) sb_add(&v, "vec3 e=normalize(ep.xyz);vec3 r=reflect(e,n);float m=2.0*sqrt(r.x*r.x+r.y*r.y+(r.z+1.0)*(r.z+1.0));");
    for (int c = 0; c < 4; c++) {
      if (!S.texgen[u][c]) continue;
      char cc = "xyzw"[c];
      switch (S.texgen_mode[u][c]) {
        case GL_OBJECT_LINEAR: sb_add(&v, "tc.%c=a_pos.%c;", cc, cc); break;
        case GL_EYE_LINEAR: sb_add(&v, "tc.%c=ep.%c;", cc, cc); break;
        case GL_SPHERE_MAP: sb_add(&v, "tc.%c=%s;", cc, c == 0 ? "r.x/m+0.5" : c == 1 ? "r.y/m+0.5" : "0.0"); break;
        case GL_REFLECTION_MAP: sb_add(&v, "tc.%c=r.%c;", cc, cc); break;
        case GL_NORMAL_MAP: sb_add(&v, "tc.%c=n.%c;", cc, c < 3 ? cc : 'x'); break;
      }
    }
    sb_add(&v, "v_tc%d=u_tm[%d]*tc;}\n", u, u);
  }
  sb_add(&v, "}\n");
  (void)any_eye;

  sb_add(&f, "#version 300 es\nprecision highp float;\n"
             "%sin vec4 v_col;in vec4 v_tc0;in vec4 v_tc1;in float v_fogz;\n"
             "uniform sampler2D u_tex[%d];uniform float u_aref;uniform vec4 u_fogc;uniform vec3 u_fogp;uniform vec4 u_envc[%d];\n"
             "out vec4 o_col;\nvoid main(){\nvec4 c=v_col;\n",
         flat, MAX_UNITS, MAX_UNITS);
  for (int u = 0; u < MAX_UNITS; u++) {
    if (!units_on[u]) continue;
    int fmt = tex_fmt(u);
    sb_add(&f, "vec4 t%d=textureProj(u_tex[%d],v_tc%d.xyw);\n", u, u, u);
    int has_rgb = fmt != FMT_ALPHA, has_a = fmt == FMT_RGBA || fmt == FMT_ALPHA || fmt == FMT_LUMA || fmt == FMT_INT;
    char t[8];
    snprintf(t, sizeof t, "t%d", u);
    switch (S.env_mode[u]) {
      case GL_REPLACE:
        if (has_rgb) sb_add(&f, "c.rgb=%s.rgb;", t);
        if (has_a) sb_add(&f, "c.a=%s.a;", t);
        break;
      case GL_MODULATE:
        if (has_rgb) sb_add(&f, "c.rgb*=%s.rgb;", t);
        if (has_a) sb_add(&f, "c.a*=%s.a;", t);
        break;
      case GL_DECAL:
        if (fmt == FMT_RGBA) sb_add(&f, "c.rgb=mix(c.rgb,%s.rgb,%s.a);", t, t);
        else if (has_rgb) sb_add(&f, "c.rgb=%s.rgb;", t);
        break;
      case GL_BLEND:
        if (has_rgb) sb_add(&f, "c.rgb=mix(c.rgb,u_envc[%d].rgb,%s.rgb);", u, t);
        if (fmt == FMT_INT) sb_add(&f, "c.a=mix(c.a,u_envc[%d].a,%s.a);", u, t);
        else if (has_a) sb_add(&f, "c.a*=%s.a;", t);
        break;
      case GL_ADD:
        if (has_rgb) sb_add(&f, "c.rgb+=%s.rgb;", t);
        if (fmt == FMT_INT) sb_add(&f, "c.a+=%s.a;", t);
        else if (has_a) sb_add(&f, "c.a*=%s.a;", t);
        break;
      case GL_COMBINE: {
        sb_add(&f, "{");
        if (!has_a) sb_add(&f, "%s.a=1.0;", t);
        if (fmt == FMT_ALPHA) sb_add(&f, "%s.rgb=vec3(0.0);", t);
        comb_arg(&f, "r0", S.src_rgb[u][0], S.op_rgb[u][0], u, 0);
        comb_arg(&f, "r1", S.src_rgb[u][1], S.op_rgb[u][1], u, 0);
        comb_arg(&f, "r2", S.src_rgb[u][2], S.op_rgb[u][2], u, 0);
        comb_arg(&f, "a0", S.src_a[u][0], S.op_a[u][0], u, 1);
        comb_arg(&f, "a1", S.src_a[u][1], S.op_a[u][1], u, 1);
        comb_arg(&f, "a2", S.src_a[u][2], S.op_a[u][2], u, 1);
        sb_add(&f, "vec3 rr;float ra;");
        comb_fn(&f, "rr", S.comb_rgb[u], "r0", "r1", "r2", 0);
        comb_fn(&f, "ra", S.comb_a[u], "a0", "a1", "a2", 1);
        sb_add(&f, "c=clamp(vec4(rr*%f,ra*%f),0.0,1.0);}", S.rgb_scale[u], S.a_scale[u]);
        break;
      }
    }
    sb_add(&f, "\n");
  }
  if (S.alpha_test) sb_add(&f, "{float a=c.a;if(!(%s))discard;}\n", alpha_cmp(S.alpha_func));
  if (S.fog) {
    if (S.fog_mode == GL_LINEAR) sb_add(&f, "float ff=(u_fogp.y-v_fogz)/max(u_fogp.y-u_fogp.x,1e-6);");
    else if (S.fog_mode == GL_EXP) sb_add(&f, "float ff=exp(-u_fogp.z*v_fogz);");
    else sb_add(&f, "float ff=exp(-(u_fogp.z*v_fogz)*(u_fogp.z*v_fogz));");
    sb_add(&f, "c.rgb=mix(u_fogc.rgb,c.rgb,clamp(ff,0.0,1.0));\n");
  }
  sb_add(&f, "o_col=c;}\n");

  GLuint vs = compile(GL_VERTEX_SHADER, v.p), fs = compile(GL_FRAGMENT_SHADER, f.p);
  GLuint p = glCreateProgram();
  glAttachShader(p, vs);
  glAttachShader(p, fs);
  glLinkProgram(p);
  GLint ok = 0;
  glGetProgramiv(p, GL_LINK_STATUS, &ok);
  if (!ok) {
    char log[2048];
    glGetProgramInfoLog(p, sizeof log, NULL, log);
    fprintf(stderr, "link error: %s\n%s\n%s\n", log, v.p, f.p);
    fatal("GL program link failed");
  }
  glDeleteShader(vs);
  glDeleteShader(fs);
  free(v.p);
  free(f.p);
  P->prog = p;
#define U(n) P->n = glGetUniformLocation(p, #n)
  U(u_mvp); U(u_mv); U(u_nm);
  U(u_lamb); U(u_ldif); U(u_lspec); U(u_lpos); U(u_lspot); U(u_lspotp); U(u_latt); U(u_lmamb);
  U(u_mamb); U(u_mdif); U(u_mspec); U(u_memi); U(u_mshin);
  U(u_fogc); U(u_fogp); U(u_aref); U(u_envc);
#undef U
  for (int u = 0; u < MAX_UNITS; u++) {
    char nm[16];
    snprintf(nm, sizeof nm, "u_tm[%d]", u);
    P->u_tm[u] = glGetUniformLocation(p, nm);
    snprintf(nm, sizeof nm, "u_tex[%d]", u);
    P->u_tex[u] = glGetUniformLocation(p, nm);
  }
  glUseProgram(p);
  for (int u = 0; u < MAX_UNITS; u++)
    if (P->u_tex[u] >= 0) glUniform1i(P->u_tex[u], u);
  LOGV("GL: compiled program %d key %016llx %016llx\n", nprogs, (unsigned long long)key[0], (unsigned long long)key[1]);
  return last = P;
}

static Mat *cur_mat(void) {
  switch (S.matrix_mode) {
    case GL_PROJECTION: return &S.proj[S.proj_sp];
    case GL_TEXTURE: return &S.tex[S.active_unit][S.tex_sp[S.active_unit]];
    default: return &S.mv[S.mv_sp];
  }
}

static void set_uniforms(Prog *P) {
  Mat *mv = &S.mv[S.mv_sp], *pj = &S.proj[S.proj_sp];
  Mat mvp;
  mat_mul(&mvp, pj, mv);
  glUniformMatrix4fv(P->u_mvp, 1, GL_FALSE, mvp.m);
  glUniformMatrix4fv(P->u_mv, 1, GL_FALSE, mv->m);
  if (P->u_nm >= 0) {
    float nm[9];
    mat_inv3(mv, nm);
    glUniformMatrix3fv(P->u_nm, 1, GL_FALSE, nm);
  }
  for (int u = 0; u < MAX_UNITS; u++)
    if (P->u_tm[u] >= 0) glUniformMatrix4fv(P->u_tm[u], 1, GL_FALSE, S.tex[u][S.tex_sp[u]].m);
  if (P->u_lamb >= 0) {
    float spotp[8][4];
    for (int i = 0; i < 8; i++) {
      spotp[i][0] = S.lspot_exp[i];
      spotp[i][1] = S.lspot_cut[i] >= 180.0f ? -2.0f : cosf(S.lspot_cut[i] * 3.14159265f / 180.0f);
      spotp[i][2] = spotp[i][3] = 0;
    }
    glUniform4fv(P->u_lamb, 8, &S.lamb[0][0]);
    glUniform4fv(P->u_ldif, 8, &S.ldif[0][0]);
    glUniform4fv(P->u_lspec, 8, &S.lspec[0][0]);
    glUniform4fv(P->u_lpos, 8, &S.lpos[0][0]);
    glUniform3fv(P->u_lspot, 8, &S.lspot[0][0]);
    glUniform4fv(P->u_lspotp, 8, &spotp[0][0]);
    glUniform3fv(P->u_latt, 8, &S.latt[0][0]);
    glUniform4fv(P->u_lmamb, 1, S.lm_amb);
    glUniform4fv(P->u_mamb, 1, S.mamb);
    glUniform4fv(P->u_mdif, 1, S.mdif);
    glUniform4fv(P->u_mspec, 1, S.mspec);
    glUniform4fv(P->u_memi, 1, S.memi);
    glUniform1f(P->u_mshin, S.mshin);
  }
  if (P->u_fogc >= 0) {
    glUniform4fv(P->u_fogc, 1, S.fog_color);
    glUniform3f(P->u_fogp, S.fog_start, S.fog_end, S.fog_density);
  }
  if (P->u_aref >= 0) glUniform1f(P->u_aref, S.alpha_ref);
  if (P->u_envc >= 0) glUniform4fv(P->u_envc, MAX_UNITS, &S.env_color[0][0]);
}

/* ------------------------------------------------------------------ vertex upload */
static GLuint vbo[8], ibo;
static GLuint vao;

static int type_size(int t) {
  switch (t) {
    case GL_BYTE: case GL_UNSIGNED_BYTE: return 1;
    case GL_SHORT: case GL_UNSIGNED_SHORT: return 2;
    case GL_DOUBLE: return 8;
    default: return 4;
  }
}

static float *conv_buf;
static size_t conv_cap;

/* uploads vertices [first, first+count) of a client array to attribute slot `loc` */
static void upload_attr(int loc, int size, int type, int stride, uint32_t ptr, int first, int count, int normalized) {
  int es = type_size(type) * size;
  if (!stride) stride = es;
  glBindBuffer(GL_ARRAY_BUFFER, vbo[loc]);
  if (type == GL_DOUBLE) {
    size_t need = (size_t)count * size * sizeof(float);
    if (need > conv_cap) { conv_cap = need * 2; conv_buf = realloc(conv_buf, conv_cap); }
    for (int i = 0; i < count; i++)
      for (int c = 0; c < size; c++) conv_buf[i * size + c] = (float)LDF64(ptr + (uint32_t)(first + i) * stride + c * 8);
    glBufferData(GL_ARRAY_BUFFER, need, conv_buf, GL_STREAM_DRAW);
    glVertexAttribPointer(loc, size, GL_FLOAT, GL_FALSE, 0, 0);
  } else {
    uint32_t start = ptr + (uint32_t)first * stride;
    uint32_t bytes = (uint32_t)(count - 1) * stride + es;
    glBufferData(GL_ARRAY_BUFFER, bytes, GPTR(start), GL_STREAM_DRAW);
    glVertexAttribPointer(loc, size, type, normalized ? GL_TRUE : GL_FALSE, stride, 0);
  }
  glEnableVertexAttribArray(loc);
}

static void setup_arrays(int first, int count) {
  glBindVertexArray(vao);
  if (S.va.enabled) upload_attr(A_POS, S.va.size, S.va.type, S.va.stride, S.va.ptr, first, count, 0);
  else { glDisableVertexAttribArray(A_POS); glVertexAttrib4f(A_POS, 0, 0, 0, 1); }
  if (S.na.enabled) upload_attr(A_NRM, 3, S.na.type, S.na.stride, S.na.ptr, first, count, S.na.type != GL_FLOAT);
  else { glDisableVertexAttribArray(A_NRM); glVertexAttrib4f(A_NRM, S.cur_normal[0], S.cur_normal[1], S.cur_normal[2], 0); }
  if (S.ca.enabled) upload_attr(A_COL, S.ca.size, S.ca.type, S.ca.stride, S.ca.ptr, first, count, S.ca.type != GL_FLOAT);
  else { glDisableVertexAttribArray(A_COL); glVertexAttrib4fv(A_COL, S.cur_color); }
  for (int u = 0; u < MAX_UNITS; u++) {
    if (S.ta[u].enabled && unit_on(u)) upload_attr(A_TC0 + u, S.ta[u].size, S.ta[u].type, S.ta[u].stride, S.ta[u].ptr, first, count, 0);
    else { glDisableVertexAttribArray(A_TC0 + u); glVertexAttrib4fv(A_TC0 + u, S.cur_tc[u]); }
  }
}

int g_gl_trace; /* set from the console (Module._gl_trace(n)) to log the draws of n frames */
EMSCRIPTEN_KEEPALIVE void gl_trace(int frames) { g_gl_trace = frames; }

static void trace_draw(const char *what, int mode, int count) {
  fprintf(stderr, "%s mode=%d n=%d lit=%d lights=%d%d%d%d cm=%d/%04x tex=%d:%u(fmt%d env%04x gen%d%d) %d:%u(fmt%d env%04x) arrays v%d/%x/%d n%d/%x c%d/%d/%x t%d/%d/%x blend=%d atest=%d/%04x/%.2f fog=%d curcol=%.2f,%.2f,%.2f,%.2f\n",
          what, mode, count, S.lighting, S.light_en[0], S.light_en[1], S.light_en[2], S.light_en[3], S.color_material, S.cm_mode,
          S.tex2d[0], S.bound[0], tex_fmt(0), S.env_mode[0], S.texgen[0][0], S.texgen_mode[0][0] & 0xf,
          S.tex2d[1], S.bound[1], tex_fmt(1), S.env_mode[1],
          S.va.enabled, S.va.type, S.va.size, S.na.enabled, S.na.type, S.ca.enabled, S.ca.size, S.ca.type,
          S.ta[0].enabled, S.ta[0].size, S.ta[0].type, S.blend, S.alpha_test, S.alpha_func, S.alpha_ref, S.fog,
          S.cur_color[0], S.cur_color[1], S.cur_color[2], S.cur_color[3]);
  if (S.lighting) {
    for (int i = 0; i < 8; i++)
      if (S.light_en[i])
        fprintf(stderr, "   light%d amb %.2f %.2f %.2f dif %.2f %.2f %.2f pos %.2f %.2f %.2f %.2f | lmamb %.2f mdif %.2f %.2f %.2f mamb %.2f\n", i,
                S.lamb[i][0], S.lamb[i][1], S.lamb[i][2], S.ldif[i][0], S.ldif[i][1], S.ldif[i][2],
                S.lpos[i][0], S.lpos[i][1], S.lpos[i][2], S.lpos[i][3], S.lm_amb[0], S.mdif[0], S.mdif[1], S.mdif[2], S.mamb[0]);
  }
}

static void prepare_draw(void) {
  Prog *P = get_prog();
  glUseProgram(P->prog);
  set_uniforms(P);
  for (int u = MAX_UNITS - 1; u >= 0; u--) {
    glActiveTexture(GL_TEXTURE0 + u);
    glBindTexture(GL_TEXTURE_2D, unit_on(u) ? texs[S.bound[u]].id : 0);
  }
}

/* primitive conversion of GL 1.1 types WebGL lacks into indexed triangles */
static uint32_t *idx_buf;
static size_t idx_cap;
static uint32_t *idx_reserve(size_t n) {
  if (n > idx_cap) { idx_cap = n * 2; idx_buf = realloc(idx_buf, idx_cap * 4); }
  return idx_buf;
}

/* fills idx_buf with triangle indices for `mode` over `count` source indices;
 * src==NULL means 0..count-1. Returns index count and GL mode to draw. */
static int convert_prims(int mode, const uint32_t *src, int count, int *out_mode) {
#define SI(i) (src ? src[i] : (uint32_t)(i))
  int n = 0;
  if (mode == GL_QUADS) {
    int q = count / 4;
    uint32_t *o = idx_reserve((size_t)q * 6);
    for (int i = 0; i < q; i++) {
      uint32_t a = SI(i * 4), b = SI(i * 4 + 1), c = SI(i * 4 + 2), d = SI(i * 4 + 3);
      o[n++] = a; o[n++] = b; o[n++] = c; o[n++] = a; o[n++] = c; o[n++] = d;
    }
    *out_mode = GL_TRIANGLES;
  } else if (mode == GL_QUAD_STRIP) {
    int q = count >= 4 ? (count - 2) / 2 : 0;
    uint32_t *o = idx_reserve((size_t)q * 6);
    for (int i = 0; i < q; i++) {
      uint32_t a = SI(i * 2), b = SI(i * 2 + 1), c = SI(i * 2 + 3), d = SI(i * 2 + 2);
      o[n++] = a; o[n++] = b; o[n++] = c; o[n++] = a; o[n++] = c; o[n++] = d;
    }
    *out_mode = GL_TRIANGLES;
  } else if (mode == GL_POLYGON) {
    uint32_t *o = idx_reserve(count);
    for (int i = 0; i < count; i++) o[n++] = SI(i);
    *out_mode = GL_TRIANGLE_FAN;
  } else {
    uint32_t *o = idx_reserve(count);
    for (int i = 0; i < count; i++) o[n++] = SI(i);
    *out_mode = mode;
  }
  return n;
#undef SI
}

static void draw_indexed_u32(int mode, const uint32_t *idx, int n, int base) {
  if (n <= 0) return;
  glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ibo);
  if (base) for (int i = 0; i < n; i++) ((uint32_t *)idx)[i] -= (uint32_t)base;
  glBufferData(GL_ELEMENT_ARRAY_BUFFER, (size_t)n * 4, idx, GL_STREAM_DRAW);
  glDrawElements(mode, n, GL_UNSIGNED_INT, 0);
}

static int draw_ok(void) { return ctx_ok && S.va.enabled && S.polygon_mode != GL_POINT; }

static void do_draw_arrays(int mode, int first, int count) {
  if (!draw_ok() || count <= 0) return;
  if (g_gl_trace) trace_draw("DrawArrays", mode, count);
  setup_arrays(first, count);
  prepare_draw();
  if (mode == GL_QUADS || mode == GL_QUAD_STRIP || mode == GL_POLYGON) {
    int m;
    int n = convert_prims(mode, NULL, count, &m);
    draw_indexed_u32(m, idx_buf, n, 0);
  } else {
    glDrawArrays(mode, 0, count);
  }
}

static void do_draw_elements(int mode, int count, int type, uint32_t indices) {
  if (!draw_ok() || count <= 0) return;
  if (g_gl_trace) trace_draw("DrawElements", mode, count);
  uint32_t *src = idx_reserve((size_t)count * 2);
  uint32_t *tmp = idx_buf + count; /* second half */
  (void)src;
  uint32_t mn = 0xffffffffu, mx = 0;
  for (int i = 0; i < count; i++) {
    uint32_t v = type == GL_UNSIGNED_SHORT ? LD16(indices + 2 * i) : type == GL_UNSIGNED_BYTE ? LD8(indices + i) : LD32(indices + 4 * i);
    tmp[i] = v;
    if (v < mn) mn = v;
    if (v > mx) mx = v;
  }
  setup_arrays((int)mn, (int)(mx - mn + 1));
  prepare_draw();
  /* convert (may realloc idx_buf, so copy the source first) */
  uint32_t *copy = malloc((size_t)count * 4);
  memcpy(copy, tmp, (size_t)count * 4);
  int m;
  int n = convert_prims(mode, copy, count, &m);
  free(copy);
  draw_indexed_u32(m, idx_buf, n, (int)mn);
}

/* ------------------------------------------------------------------ immediate mode */
typedef struct { float pos[4], col[4], tc[MAX_UNITS][4], nrm[3]; } IVert;
static IVert *imm;
static int imm_n, imm_cap, imm_mode = -1;

static void imm_vertex(float x, float y, float z, float w) {
  if (imm_mode < 0) return;
  if (imm_n >= imm_cap) { imm_cap = imm_cap ? imm_cap * 2 : 1024; imm = realloc(imm, sizeof(IVert) * imm_cap); }
  IVert *v = &imm[imm_n++];
  v->pos[0] = x; v->pos[1] = y; v->pos[2] = z; v->pos[3] = w;
  memcpy(v->col, S.cur_color, 16);
  memcpy(v->tc, S.cur_tc, sizeof v->tc);
  memcpy(v->nrm, S.cur_normal, 12);
}

static void imm_flush(void) {
  int mode = imm_mode;
  imm_mode = -1;
  if (!ctx_ok || imm_n == 0) return;
  if (g_gl_trace) trace_draw("Immediate", mode, imm_n);
  glBindVertexArray(vao);
  size_t st = sizeof(IVert);
  glBindBuffer(GL_ARRAY_BUFFER, vbo[7]);
  glBufferData(GL_ARRAY_BUFFER, st * imm_n, imm, GL_STREAM_DRAW);
  glVertexAttribPointer(A_POS, 4, GL_FLOAT, GL_FALSE, (GLsizei)st, (void *)offsetof(IVert, pos));
  glEnableVertexAttribArray(A_POS);
  glVertexAttribPointer(A_COL, 4, GL_FLOAT, GL_FALSE, (GLsizei)st, (void *)offsetof(IVert, col));
  glEnableVertexAttribArray(A_COL);
  glVertexAttribPointer(A_NRM, 3, GL_FLOAT, GL_FALSE, (GLsizei)st, (void *)offsetof(IVert, nrm));
  glEnableVertexAttribArray(A_NRM);
  for (int u = 0; u < MAX_UNITS; u++) {
    glVertexAttribPointer(A_TC0 + u, 4, GL_FLOAT, GL_FALSE, (GLsizei)st, (void *)(offsetof(IVert, tc) + 16 * u));
    glEnableVertexAttribArray(A_TC0 + u);
  }
  prepare_draw();
  if (mode == GL_QUADS || mode == GL_QUAD_STRIP || mode == GL_POLYGON) {
    int m;
    int n = convert_prims(mode, NULL, imm_n, &m);
    draw_indexed_u32(m, idx_buf, n, 0);
  } else {
    glDrawArrays(mode, 0, imm_n);
  }
  imm_n = 0;
}

/* ------------------------------------------------------------------ context / present */
static void apply_viewport(void) {
  glViewport((int)lrintf(S.vp[0] * scale_x), (int)lrintf(S.vp[1] * scale_y), (int)lrintf(S.vp[2] * scale_x), (int)lrintf(S.vp[3] * scale_y));
}
static void apply_scissor(void) {
  glScissor((int)lrintf(S.sc[0] * scale_x), (int)lrintf(S.sc[1] * scale_y), (int)lrintf(S.sc[2] * scale_x), (int)lrintf(S.sc[3] * scale_y));
}

static void update_scale(void) {
  canvas_w = js_canvas_width();
  canvas_h = js_canvas_height();
  scale_x = (float)canvas_w / (float)game_w;
  scale_y = (float)canvas_h / (float)game_h;
  if (ctx_ok) { apply_viewport(); apply_scissor(); }
}

void gl_set_game_size(int w, int h) {
  if (w <= 0 || h <= 0) return;
  game_w = w;
  game_h = h;
  js_set_canvas_size(w, h);
  update_scale();
}

static void reset_state(void) {
  memset(&S, 0, sizeof S);
  for (int i = 0; i < MV_DEPTH; i++) mat_ident(&S.mv[i]);
  for (int i = 0; i < OTHER_DEPTH; i++) {
    mat_ident(&S.proj[i]);
    for (int u = 0; u < MAX_UNITS; u++) mat_ident(&S.tex[u][i]);
  }
  S.matrix_mode = GL_MODELVIEW;
  for (int i = 0; i < 8; i++) {
    S.lamb[i][3] = 1;
    S.ldif[i][3] = S.lspec[i][3] = 1;
    if (i == 0) { S.ldif[0][0] = S.ldif[0][1] = S.ldif[0][2] = 1; S.lspec[0][0] = S.lspec[0][1] = S.lspec[0][2] = 1; }
    S.lpos[i][2] = 1;
    S.lspot[i][2] = -1;
    S.lspot_cut[i] = 180;
    S.latt[i][0] = 1;
  }
  S.lm_amb[0] = S.lm_amb[1] = S.lm_amb[2] = 0.2f; S.lm_amb[3] = 1;
  S.mamb[0] = S.mamb[1] = S.mamb[2] = 0.2f; S.mamb[3] = 1;
  S.mdif[0] = S.mdif[1] = S.mdif[2] = 0.8f; S.mdif[3] = 1;
  S.mspec[3] = 1; S.memi[3] = 1;
  S.cm_mode = GL_AMBIENT_AND_DIFFUSE;
  S.fog_mode = GL_EXP; S.fog_density = 1; S.fog_end = 1;
  S.alpha_func = GL_ALWAYS;
  for (int u = 0; u < MAX_UNITS; u++) {
    S.env_mode[u] = GL_MODULATE;
    S.comb_rgb[u] = S.comb_a[u] = GL_MODULATE;
    S.src_rgb[u][0] = S.src_a[u][0] = GL_TEXTURE;
    S.src_rgb[u][1] = S.src_a[u][1] = GL_PREVIOUS;
    S.src_rgb[u][2] = S.src_a[u][2] = GL_CONSTANT;
    S.op_rgb[u][0] = S.op_rgb[u][1] = GL_SRC_COLOR;
    S.op_rgb[u][2] = GL_SRC_ALPHA;
    S.op_a[u][0] = S.op_a[u][1] = S.op_a[u][2] = GL_SRC_ALPHA;
    S.rgb_scale[u] = S.a_scale[u] = 1;
    S.cur_tc[u][3] = 1;
    for (int c = 0; c < 4; c++) S.texgen_mode[u][c] = GL_EYE_LINEAR;
  }
  S.cur_color[0] = S.cur_color[1] = S.cur_color[2] = S.cur_color[3] = 1;
  S.cur_normal[2] = 1;
  S.va.size = S.ca.size = 4;
  S.va.type = S.na.type = S.ca.type = GL_FLOAT;
  for (int u = 0; u < MAX_UNITS; u++) { S.ta[u].size = 4; S.ta[u].type = GL_FLOAT; }
  S.vp[2] = game_w; S.vp[3] = game_h;
  S.sc[2] = game_w; S.sc[3] = game_h;
  S.polygon_mode = GL_FILL;
}

static void create_context(void) {
  if (ctx_ok) return;
  EmscriptenWebGLContextAttributes a;
  emscripten_webgl_init_context_attributes(&a);
  a.majorVersion = 2;
  a.minorVersion = 0;
  a.alpha = 0;
  a.depth = 1;
  a.stencil = 1;
  a.antialias = 1;
  a.preserveDrawingBuffer = 0;
  a.powerPreference = EM_WEBGL_POWER_PREFERENCE_HIGH_PERFORMANCE;
  ctx = emscripten_webgl_create_context("#canvas", &a);
  if (ctx <= 0) fatal("WebGL2 is not available in this browser");
  emscripten_webgl_make_context_current(ctx);
  ctx_ok = 1;
  glGenVertexArrays(1, &vao);
  glBindVertexArray(vao);
  glGenBuffers(8, vbo);
  glGenBuffers(1, &ibo);
  glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
  reset_state();
  update_scale();
  glDisable(GL_DITHER);
  if (emscripten_webgl_enable_extension(ctx, "EXT_texture_filter_anisotropic")) {
    glGetFloatv(GL_MAX_TEXTURE_MAX_ANISOTROPY, &aniso_max);
    if (aniso_max > 16) aniso_max = 16;
    if (aniso_max < 1) aniso_max = 1;
  }
}

void gl_present(void) {
  if (imm_mode >= 0) imm_flush();
  if (g_gl_trace) { g_gl_trace--; fprintf(stderr, "---- end of frame\n"); }
  js_present();
  int w = js_canvas_width(), h = js_canvas_height();
  if (w != canvas_w || h != canvas_h) update_scale();
}

/* ------------------------------------------------------------------ wgl */
HLE(opengl32, wglCreateContext) { create_context(); RET(1, 0x00C00001u); }
HLE(opengl32, wglMakeCurrent) { RET(2, 1); }
HLE(opengl32, wglDeleteContext) { RET(1, 1); }
HLE(opengl32, wglGetCurrentDC) { RET(0, 0x00D00001u); }

static void gl_ActiveTextureARB(void) {
  uint32_t u = ARG(0) - GL_TEXTURE0_ARB;
  if (u < MAX_UNITS) S.active_unit = (int)u;
  RETV(1);
}
static void gl_ClientActiveTextureARB(void) {
  uint32_t u = ARG(0) - GL_TEXTURE0_ARB;
  if (u < MAX_UNITS) S.client_unit = (int)u;
  RETV(1);
}
static void gl_MultiTexCoord2fARB(void) {
  uint32_t u = ARG(0) - GL_TEXTURE0_ARB;
  if (u < MAX_UNITS) { S.cur_tc[u][0] = ARGF(1); S.cur_tc[u][1] = ARGF(2); S.cur_tc[u][2] = 0; S.cur_tc[u][3] = 1; }
  RETV(3);
}

HLE(opengl32, wglGetProcAddress) {
  const char *n = ARGP(0);
  uint32_t r = 0;
  if (!strcmp(n, "glActiveTextureARB")) r = hle_thunk(gl_ActiveTextureARB, "glActiveTextureARB");
  else if (!strcmp(n, "glClientActiveTextureARB")) r = hle_thunk(gl_ClientActiveTextureARB, "glClientActiveTextureARB");
  else if (!strcmp(n, "glMultiTexCoord2fARB")) r = hle_thunk(gl_MultiTexCoord2fARB, "glMultiTexCoord2fARB");
  LOG("wglGetProcAddress(%s) -> %08x\n", n, r);
  RET(1, r);
}

/* ------------------------------------------------------------------ queries */
static uint32_t str_vendor, str_renderer, str_version, str_ext;
HLE(opengl32, glGetString) {
  switch (ARG(0)) {
    case 0x1F00: RET(1, str_vendor);
    case 0x1F01: RET(1, str_renderer);
    case 0x1F02: RET(1, str_version);
    case 0x1F03: RET(1, str_ext);
  }
  RET(1, 0);
}

static void get_floats(uint32_t pname, float *out, int *n) {
  *n = 1;
  switch (pname) {
    case GL_MODELVIEW_MATRIX: memcpy(out, S.mv[S.mv_sp].m, 64); *n = 16; return;
    case GL_PROJECTION_MATRIX: memcpy(out, S.proj[S.proj_sp].m, 64); *n = 16; return;
    case GL_TEXTURE_MATRIX: memcpy(out, S.tex[S.active_unit][S.tex_sp[S.active_unit]].m, 64); *n = 16; return;
    case GL_VIEWPORT: for (int i = 0; i < 4; i++) out[i] = (float)S.vp[i]; *n = 4; return;
    case GL_SCISSOR_BOX: for (int i = 0; i < 4; i++) out[i] = (float)S.sc[i]; *n = 4; return;
    case GL_MAX_TEXTURE_SIZE: out[0] = 4096; return;
    case GL_MAX_TEXTURE_UNITS: out[0] = MAX_UNITS; return;
    case GL_MAX_TEXTURE_MAX_ANISOTROPY: out[0] = 1; return;
    case 0x0D32: out[0] = 8; return;  /* MAX_LIGHTS */
    case 0x0D36: out[0] = 32; return; /* MAX_MODELVIEW_STACK_DEPTH */
    case 0x0D38: out[0] = 8; return;  /* MAX_PROJECTION_STACK_DEPTH */
    case GL_STENCIL_BITS: out[0] = 8; return;
    case GL_DEPTH_BITS: out[0] = 24; return;
    case GL_RED_BITS: case GL_GREEN_BITS: case GL_BLUE_BITS: case GL_ALPHA_BITS: out[0] = 8; return;
    case 0x0BA0: out[0] = (float)S.matrix_mode; return;
  }
  LOG("glGet: unhandled pname %04x\n", pname);
  out[0] = 0;
}

HLE(opengl32, glGetFloatv) {
  float v[16];
  int n;
  get_floats(ARG(0), v, &n);
  for (int i = 0; i < n; i++) STF32(ARG(1) + 4 * i, v[i]);
  RETV(2);
}
HLE(opengl32, glGetIntegerv) {
  float v[16];
  int n;
  get_floats(ARG(0), v, &n);
  for (int i = 0; i < n; i++) ST32(ARG(1) + 4 * i, (uint32_t)(int32_t)v[i]);
  RETV(2);
}

/* ------------------------------------------------------------------ enables */
static void set_cap(uint32_t cap, int on) {
  if (cap >= GL_LIGHT0 && cap < GL_LIGHT0 + 8) { S.light_en[cap - GL_LIGHT0] = on; return; }
  switch (cap) {
    case GL_LIGHTING: S.lighting = on; return;
    case GL_COLOR_MATERIAL: S.color_material = on; return;
    case GL_NORMALIZE: case GL_RESCALE_NORMAL: S.normalize = on; return;
    case GL_ALPHA_TEST: S.alpha_test = on; return;
    case GL_FOG: S.fog = on; return;
    case GL_TEXTURE_2D: S.tex2d[S.active_unit] = on; return;
    case GL_TEXTURE_GEN_S: S.texgen[S.active_unit][0] = on; return;
    case GL_TEXTURE_GEN_T: S.texgen[S.active_unit][1] = on; return;
    case GL_TEXTURE_GEN_R: S.texgen[S.active_unit][2] = on; return;
    case GL_TEXTURE_GEN_Q: S.texgen[S.active_unit][3] = on; return;
    case GL_CULL_FACE: S.cull = on; break;
    case GL_DEPTH_TEST: S.depth_test = on; break;
    case GL_BLEND: S.blend = on; break;
    case GL_SCISSOR_TEST: S.scissor = on; break;
    case GL_STENCIL_TEST: S.stencil = on; break;
    case GL_POLYGON_OFFSET_FILL: case GL_DITHER: break;
    default: return; /* line/point smooth, multisample, cube maps, ... */
  }
  if (!ctx_ok) return;
  if (cap == GL_DITHER) return;
  if (on) glEnable(cap); else glDisable(cap);
}
HLE(opengl32, glEnable) { set_cap(ARG(0), 1); RETV(1); }
HLE(opengl32, glDisable) { set_cap(ARG(0), 0); RETV(1); }

HLE(opengl32, glEnableClientState) {
  switch (ARG(0)) {
    case GL_VERTEX_ARRAY: S.va.enabled = 1; break;
    case GL_NORMAL_ARRAY: S.na.enabled = 1; break;
    case GL_COLOR_ARRAY: S.ca.enabled = 1; break;
    case GL_TEXTURE_COORD_ARRAY: S.ta[S.client_unit].enabled = 1; break;
  }
  RETV(1);
}
HLE(opengl32, glDisableClientState) {
  switch (ARG(0)) {
    case GL_VERTEX_ARRAY: S.va.enabled = 0; break;
    case GL_NORMAL_ARRAY: S.na.enabled = 0; break;
    case GL_COLOR_ARRAY: S.ca.enabled = 0; break;
    case GL_TEXTURE_COORD_ARRAY: S.ta[S.client_unit].enabled = 0; break;
  }
  RETV(1);
}

HLE(opengl32, glVertexPointer) { S.va.size = (int)ARG(0); S.va.type = (int)ARG(1); S.va.stride = (int)ARG(2); S.va.ptr = ARG(3); RETV(4); }
HLE(opengl32, glNormalPointer) { S.na.size = 3; S.na.type = (int)ARG(0); S.na.stride = (int)ARG(1); S.na.ptr = ARG(2); RETV(3); }
HLE(opengl32, glColorPointer) { S.ca.size = (int)ARG(0); S.ca.type = (int)ARG(1); S.ca.stride = (int)ARG(2); S.ca.ptr = ARG(3); RETV(4); }
HLE(opengl32, glTexCoordPointer) {
  int u = S.client_unit;
  S.ta[u].size = (int)ARG(0); S.ta[u].type = (int)ARG(1); S.ta[u].stride = (int)ARG(2); S.ta[u].ptr = ARG(3);
  RETV(4);
}

HLE(opengl32, glDrawArrays) { do_draw_arrays((int)ARG(0), (int)ARG(1), (int)ARG(2)); RETV(3); }
HLE(opengl32, glDrawElements) { do_draw_elements((int)ARG(0), (int)ARG(1), (int)ARG(2), ARG(3)); RETV(4); }

/* ------------------------------------------------------------------ immediate mode */
HLE(opengl32, glBegin) { imm_mode = (int)ARG(0); imm_n = 0; RETV(1); }
HLE(opengl32, glEnd) { imm_flush(); RETV(0); }
HLE(opengl32, glVertex2f) { imm_vertex(ARGF(0), ARGF(1), 0, 1); RETV(2); }
HLE(opengl32, glVertex2i) { imm_vertex((float)(int32_t)ARG(0), (float)(int32_t)ARG(1), 0, 1); RETV(2); }
HLE(opengl32, glVertex3i) { imm_vertex((float)(int32_t)ARG(0), (float)(int32_t)ARG(1), (float)(int32_t)ARG(2), 1); RETV(3); }
HLE(opengl32, glVertex2fv) { uint32_t p = ARG(0); imm_vertex(LDF32(p), LDF32(p + 4), 0, 1); RETV(1); }
HLE(opengl32, glVertex3fv) { uint32_t p = ARG(0); imm_vertex(LDF32(p), LDF32(p + 4), LDF32(p + 8), 1); RETV(1); }
HLE(opengl32, glTexCoord2f) { int u = 0; S.cur_tc[u][0] = ARGF(0); S.cur_tc[u][1] = ARGF(1); S.cur_tc[u][2] = 0; S.cur_tc[u][3] = 1; RETV(2); }
HLE(opengl32, glTexCoord2fv) { uint32_t p = ARG(0); S.cur_tc[0][0] = LDF32(p); S.cur_tc[0][1] = LDF32(p + 4); S.cur_tc[0][2] = 0; S.cur_tc[0][3] = 1; RETV(1); }
static void set_color_ub(uint32_t r, uint32_t g, uint32_t b, uint32_t a) {
  S.cur_color[0] = (r & 0xff) / 255.0f; S.cur_color[1] = (g & 0xff) / 255.0f;
  S.cur_color[2] = (b & 0xff) / 255.0f; S.cur_color[3] = (a & 0xff) / 255.0f;
}
HLE(opengl32, glColor4ub) { set_color_ub(ARG(0), ARG(1), ARG(2), ARG(3)); RETV(4); }
HLE(opengl32, glColor4ubv) { uint32_t p = ARG(0); set_color_ub(LD8(p), LD8(p + 1), LD8(p + 2), LD8(p + 3)); RETV(1); }
HLE(opengl32, glColor3ubv) { uint32_t p = ARG(0); set_color_ub(LD8(p), LD8(p + 1), LD8(p + 2), 255); RETV(1); }

/* ------------------------------------------------------------------ matrices */
HLE(opengl32, glMatrixMode) { S.matrix_mode = (int)ARG(0); RETV(1); }
HLE(opengl32, glLoadIdentity) { mat_ident(cur_mat()); RETV(0); }
HLE(opengl32, glLoadMatrixf) { memcpy(cur_mat()->m, GPTR(ARG(0)), 64); RETV(1); }
HLE(opengl32, glMultMatrixf) {
  Mat b;
  memcpy(b.m, GPTR(ARG(0)), 64);
  Mat *m = cur_mat();
  mat_mul(m, m, &b);
  RETV(1);
}
HLE(opengl32, glPushMatrix) {
  int *sp, max;
  Mat *base;
  if (S.matrix_mode == GL_PROJECTION) { sp = &S.proj_sp; max = OTHER_DEPTH; base = S.proj; }
  else if (S.matrix_mode == GL_TEXTURE) { sp = &S.tex_sp[S.active_unit]; max = OTHER_DEPTH; base = S.tex[S.active_unit]; }
  else { sp = &S.mv_sp; max = MV_DEPTH; base = S.mv; }
  if (*sp + 1 < max) { base[*sp + 1] = base[*sp]; (*sp)++; }
  else LOG("glPushMatrix: stack overflow\n");
  RETV(0);
}
HLE(opengl32, glPopMatrix) {
  int *sp = S.matrix_mode == GL_PROJECTION ? &S.proj_sp : S.matrix_mode == GL_TEXTURE ? &S.tex_sp[S.active_unit] : &S.mv_sp;
  if (*sp > 0) (*sp)--;
  RETV(0);
}
static void mult_cur(const float *m16) {
  Mat b;
  memcpy(b.m, m16, 64);
  Mat *m = cur_mat();
  mat_mul(m, m, &b);
}
HLE(opengl32, glOrtho) {
  double l = ARGD(0), r = ARGD(2), b = ARGD(4), t = ARGD(6), n = ARGD(8), f = ARGD(10);
  float m[16] = {0};
  m[0] = (float)(2 / (r - l)); m[5] = (float)(2 / (t - b)); m[10] = (float)(-2 / (f - n));
  m[12] = (float)(-(r + l) / (r - l)); m[13] = (float)(-(t + b) / (t - b)); m[14] = (float)(-(f + n) / (f - n)); m[15] = 1;
  mult_cur(m);
  RETV(12);
}
HLE(glu32, gluPerspective) {
  double fovy = ARGD(0), aspect = ARGD(2), zn = ARGD(4), zf = ARGD(6);
  double fv = 1.0 / tan(fovy * 3.14159265358979323846 / 360.0);
  float m[16] = {0};
  m[0] = (float)(fv / aspect); m[5] = (float)fv;
  m[10] = (float)((zf + zn) / (zn - zf)); m[11] = -1;
  m[14] = (float)(2 * zf * zn / (zn - zf));
  mult_cur(m);
  RETV(8);
}

/* ------------------------------------------------------------------ lighting / fog / material */
HLE(opengl32, glLightfv) {
  uint32_t l = ARG(0) - GL_LIGHT0, pn = ARG(1), p = ARG(2);
  if (l >= 8) RETV(3);
  float v[4];
  for (int i = 0; i < 4; i++) v[i] = LDF32(p + 4 * i);
  switch (pn) {
    case GL_AMBIENT: memcpy(S.lamb[l], v, 16); break;
    case GL_DIFFUSE: memcpy(S.ldif[l], v, 16); break;
    case GL_SPECULAR: memcpy(S.lspec[l], v, 16); break;
    case GL_POSITION: mat_xform(&S.mv[S.mv_sp], v, S.lpos[l]); break;
    case GL_SPOT_DIRECTION: {
      float d[4] = {v[0], v[1], v[2], 0}, o[4];
      mat_xform(&S.mv[S.mv_sp], d, o);
      memcpy(S.lspot[l], o, 12);
      break;
    }
    case GL_SPOT_EXPONENT: S.lspot_exp[l] = v[0]; break;
    case GL_SPOT_CUTOFF: S.lspot_cut[l] = v[0]; break;
    case GL_CONSTANT_ATTENUATION: S.latt[l][0] = v[0]; break;
    case GL_LINEAR_ATTENUATION: S.latt[l][1] = v[0]; break;
    case GL_QUADRATIC_ATTENUATION: S.latt[l][2] = v[0]; break;
  }
  RETV(3);
}
HLE(opengl32, glLightModelfv) {
  uint32_t pn = ARG(0), p = ARG(1);
  if (pn == GL_LIGHT_MODEL_AMBIENT) for (int i = 0; i < 4; i++) S.lm_amb[i] = LDF32(p + 4 * i);
  else if (pn == GL_LIGHT_MODEL_LOCAL_VIEWER) S.local_viewer = LDF32(p) != 0;
  else if (pn == GL_LIGHT_MODEL_TWO_SIDE) S.two_side = LDF32(p) != 0;
  RETV(2);
}
HLE(opengl32, glColorMaterial) { S.cm_mode = (int)ARG(1); RETV(2); }
HLE(opengl32, glFogi) { if (ARG(0) == GL_FOG_MODE) S.fog_mode = (int)ARG(1); RETV(2); }
HLE(opengl32, glFogf) {
  uint32_t pn = ARG(0);
  float v = ARGF(1);
  if (pn == GL_FOG_DENSITY) S.fog_density = v;
  else if (pn == GL_FOG_START) S.fog_start = v;
  else if (pn == GL_FOG_END) S.fog_end = v;
  else if (pn == GL_FOG_MODE) S.fog_mode = (int)v;
  RETV(2);
}
HLE(opengl32, glFogfv) {
  uint32_t pn = ARG(0), p = ARG(1);
  if (pn == GL_FOG_COLOR) for (int i = 0; i < 4; i++) S.fog_color[i] = LDF32(p + 4 * i);
  else if (pn == GL_FOG_DENSITY) S.fog_density = LDF32(p);
  else if (pn == GL_FOG_START) S.fog_start = LDF32(p);
  else if (pn == GL_FOG_END) S.fog_end = LDF32(p);
  else if (pn == GL_FOG_MODE) S.fog_mode = (int)LDF32(p);
  RETV(2);
}
HLE(opengl32, glShadeModel) { S.shade_flat = ARG(0) == GL_FLAT; RETV(1); }
HLE(opengl32, glAlphaFunc) { S.alpha_func = (int)ARG(0); S.alpha_ref = ARGF(1); RETV(2); }

HLE(opengl32, glTexGeni) {
  uint32_t c = ARG(0) - 0x2000, pn = ARG(1);
  if (c < 4 && pn == GL_TEXTURE_GEN_MODE) S.texgen_mode[S.active_unit][c] = (int)ARG(2);
  RETV(3);
}

HLE(opengl32, glTexEnvf) {
  uint32_t target = ARG(0), pn = ARG(1);
  float v = ARGF(2);
  int u = S.active_unit;
  int iv = (int)v;
  if (target != GL_TEXTURE_ENV) RETV(3);
  if (pn == GL_TEXTURE_ENV_MODE) S.env_mode[u] = iv;
  else if (pn == GL_COMBINE_RGB) S.comb_rgb[u] = iv;
  else if (pn == GL_COMBINE_ALPHA) S.comb_a[u] = iv;
  else if (pn >= GL_SOURCE0_RGB && pn < GL_SOURCE0_RGB + 3) S.src_rgb[u][pn - GL_SOURCE0_RGB] = iv;
  else if (pn >= GL_SOURCE0_ALPHA && pn < GL_SOURCE0_ALPHA + 3) S.src_a[u][pn - GL_SOURCE0_ALPHA] = iv;
  else if (pn >= GL_OPERAND0_RGB && pn < GL_OPERAND0_RGB + 3) S.op_rgb[u][pn - GL_OPERAND0_RGB] = iv;
  else if (pn >= GL_OPERAND0_ALPHA && pn < GL_OPERAND0_ALPHA + 3) S.op_a[u][pn - GL_OPERAND0_ALPHA] = iv;
  else if (pn == GL_RGB_SCALE) S.rgb_scale[u] = v;
  else if (pn == GL_ALPHA_SCALE) S.a_scale[u] = v;
  else LOG("glTexEnvf: unhandled pname %04x = %f\n", pn, v);
  RETV(3);
}

/* ------------------------------------------------------------------ raster state passthrough */
HLE(opengl32, glBlendFunc) { if (ctx_ok) glBlendFunc(ARG(0), ARG(1)); RETV(2); }
HLE(opengl32, glCullFace) { if (ctx_ok) glCullFace(ARG(0)); RETV(1); }
HLE(opengl32, glDepthMask) { if (ctx_ok) glDepthMask(ARG(0) & 0xff); RETV(1); }
HLE(opengl32, glDepthFunc) { if (ctx_ok) glDepthFunc(ARG(0)); RETV(1); }
HLE(opengl32, glColorMask) { if (ctx_ok) glColorMask(ARG(0) & 0xff, ARG(1) & 0xff, ARG(2) & 0xff, ARG(3) & 0xff); RETV(4); }
HLE(opengl32, glStencilFunc) { if (ctx_ok) glStencilFunc(ARG(0), (GLint)ARG(1), ARG(2)); RETV(3); }
HLE(opengl32, glStencilOp) { if (ctx_ok) glStencilOp(ARG(0), ARG(1), ARG(2)); RETV(3); }
HLE(opengl32, glPolygonMode) { S.polygon_mode = (int)ARG(1); RETV(2); }
HLE(opengl32, glHint) { RETV(2); }
HLE(opengl32, glPixelStorei) { if (ctx_ok && (ARG(0) == GL_UNPACK_ALIGNMENT || ARG(0) == GL_PACK_ALIGNMENT)) glPixelStorei(ARG(0), (GLint)ARG(1)); RETV(2); }
HLE(opengl32, glClearColor) { if (ctx_ok) glClearColor(ARGF(0), ARGF(1), ARGF(2), ARGF(3)); RETV(4); }
HLE(opengl32, glClearDepth) { if (ctx_ok) glClearDepthf((float)ARGD(0)); RETV(2); }
HLE(opengl32, glClear) { if (imm_mode >= 0) imm_flush(); if (ctx_ok) glClear(ARG(0) & (GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT)); RETV(1); }
HLE(opengl32, glViewport) {
  S.vp[0] = (int)ARG(0); S.vp[1] = (int)ARG(1); S.vp[2] = (int)ARG(2); S.vp[3] = (int)ARG(3);
  if (ctx_ok) apply_viewport();
  RETV(4);
}
HLE(opengl32, glScissor) {
  S.sc[0] = (int)ARG(0); S.sc[1] = (int)ARG(1); S.sc[2] = (int)ARG(2); S.sc[3] = (int)ARG(3);
  if (ctx_ok) apply_scissor();
  RETV(4);
}

/* ------------------------------------------------------------------ textures */
HLE(opengl32, glGenTextures) {
  int n = (int)ARG(0);
  uint32_t out = ARG(1);
  for (int i = 0; i < n; i++) {
    uint32_t id = 0;
    for (uint32_t k = 1; k < MAX_TEX; k++)
      if (!texs[k].used) { id = k; break; }
    if (!id) fatal("out of GL texture names");
    texs[id].used = 1;
    glGenTextures(1, &texs[id].id);
    texs[id].fmt_class = FMT_RGBA;
    texs[id].w = texs[id].h = 0;
    texs[id].min_filter = GL_NEAREST_MIPMAP_LINEAR;
    texs[id].has_mips = 0;
    glBindTexture(GL_TEXTURE_2D, texs[id].id);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST_MIPMAP_LINEAR);
    ST32(out + 4 * i, id);
  }
  RETV(2);
}

HLE(opengl32, glDeleteTextures) {
  int n = (int)ARG(0);
  uint32_t p = ARG(1);
  for (int i = 0; i < n; i++) {
    uint32_t id = LD32(p + 4 * i);
    if (id && id < MAX_TEX && texs[id].used) {
      glDeleteTextures(1, &texs[id].id);
      texs[id].used = 0;
      for (int u = 0; u < MAX_UNITS; u++) if (S.bound[u] == id) S.bound[u] = 0;
    }
  }
  RETV(2);
}

HLE(opengl32, glBindTexture) {
  uint32_t id = ARG(1);
  if (id && id < MAX_TEX && !texs[id].used) { /* names may be used without glGenTextures */
    texs[id].used = 1;
    glGenTextures(1, &texs[id].id);
    texs[id].fmt_class = FMT_RGBA;
    texs[id].min_filter = GL_NEAREST_MIPMAP_LINEAR;
    glBindTexture(GL_TEXTURE_2D, texs[id].id);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST_MIPMAP_LINEAR);
  }
  S.bound[S.active_unit] = id < MAX_TEX ? id : 0;
  RETV(2);
}

static Tex *bound_tex(void) {
  uint32_t id = S.bound[S.active_unit];
  if (!id || id >= MAX_TEX || !texs[id].used) return NULL;
  glActiveTexture(GL_TEXTURE0 + S.active_unit);
  glBindTexture(GL_TEXTURE_2D, texs[id].id);
  return &texs[id];
}

/* The game uploads only level 0 and mostly asks for plain GL_LINEAR. Every texture gets
   mipmaps generated here, sampled trilinear + anisotropic: no shimmering or jaggies on
   surfaces seen from afar. GL_NEAREST (pixel-exact art) is left alone. */
static void fix_min_filter(Tex *t) {
  GLint f = t->min_filter;
  if (t->has_mips || t->auto_mips) {
    if (f != GL_NEAREST) f = GL_LINEAR_MIPMAP_LINEAR;
  } else if (f != GL_NEAREST && f != GL_LINEAR) f = (f == GL_NEAREST_MIPMAP_NEAREST || f == GL_NEAREST_MIPMAP_LINEAR) ? GL_NEAREST : GL_LINEAR;
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, f);
  if (aniso_max > 1) glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAX_ANISOTROPY, f == GL_LINEAR_MIPMAP_LINEAR ? aniso_max : 1.0f);
}

HLE(opengl32, glTexParameteri) {
  uint32_t pn = ARG(1);
  int32_t v = (int32_t)ARG(2);
  Tex *t = bound_tex();
  if (!t) RETV(3);
  if (pn == GL_TEXTURE_WRAP_S || pn == GL_TEXTURE_WRAP_T) {
    if (v == GL_CLAMP) v = GL_CLAMP_TO_EDGE;
    glTexParameteri(GL_TEXTURE_2D, pn, v);
  } else if (pn == GL_TEXTURE_MIN_FILTER) {
    t->min_filter = v;
    fix_min_filter(t);
  } else if (pn == GL_TEXTURE_MAG_FILTER) {
    glTexParameteri(GL_TEXTURE_2D, pn, v);
  } else if (pn == GL_GENERATE_MIPMAP || pn == GL_TEXTURE_MAX_ANISOTROPY) {
  } else LOG("glTexParameteri: pname %04x\n", pn);
  RETV(3);
}

/* converts a client image to something WebGL2 accepts; returns host buffer (malloc'd or NULL if direct) */
static uint8_t *convert_image(int w, int h, uint32_t fmt, uint32_t type, uint32_t px, GLenum *ofmt, GLenum *otype, int *cls) {
  *otype = GL_UNSIGNED_BYTE;
  int align = 4;
  switch (fmt) {
    case GL_RGBA: *ofmt = GL_RGBA; *cls = FMT_RGBA; break;
    case GL_RGB: *ofmt = GL_RGB; *cls = FMT_RGB; break;
    case GL_ALPHA: *ofmt = GL_ALPHA; *cls = FMT_ALPHA; break;
    case GL_LUMINANCE: *ofmt = GL_LUMINANCE; *cls = FMT_LUM; break;
    case GL_LUMINANCE_ALPHA: *ofmt = GL_LUMINANCE_ALPHA; *cls = FMT_LUMA; break;
    case GL_BGRA: case GL_BGR: {
      int nc = fmt == GL_BGRA ? 4 : 3;
      *ofmt = nc == 4 ? GL_RGBA : GL_RGB;
      *cls = nc == 4 ? FMT_RGBA : FMT_RGB;
      if (type != GL_UNSIGNED_BYTE) break;
      uint8_t *o = malloc((size_t)w * h * nc);
      int rowb = w * nc;
      int stride = (rowb + align - 1) & ~(align - 1);
      for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
          const uint8_t *s = (uint8_t *)GPTR(px + (uint32_t)(y * stride + x * nc));
          uint8_t *d = o + (y * w + x) * nc;
          d[0] = s[2]; d[1] = s[1]; d[2] = s[0];
          if (nc == 4) d[3] = s[3];
        }
      glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
      return o;
    }
    default: *ofmt = GL_RGBA; *cls = FMT_RGBA; LOG("glTexImage2D: unhandled format %04x\n", fmt); break;
  }
  if (type == GL_UNSIGNED_SHORT_4_4_4_4 || type == GL_UNSIGNED_SHORT_5_5_5_1 || type == GL_UNSIGNED_SHORT_5_6_5) *otype = type;
  else if (type != GL_UNSIGNED_BYTE) LOG("glTexImage2D: unhandled type %04x\n", type);
  return NULL;
}

static void tex_image(int level, int w, int h, uint32_t fmt, uint32_t type, uint32_t px, int internal) {
  Tex *t = bound_tex();
  if (!t) return;
  GLenum ofmt, otype;
  int cls;
  GLint unpack = 4;
  glGetIntegerv(GL_UNPACK_ALIGNMENT, &unpack);
  uint8_t *conv = px ? convert_image(w, h, fmt, type, px, &ofmt, &otype, &cls) : NULL;
  if (!px) convert_image(w, h, fmt == GL_BGRA ? GL_RGBA : fmt, type, 0, &ofmt, &otype, &cls);
  /* GL_INTENSITY-like internal formats keep alpha = luminance */
  if (internal == GL_INTENSITY || internal == 0x804B /* INTENSITY8 */) cls = FMT_INT;
  if (internal == 1 && cls == FMT_RGBA) cls = FMT_LUM;
  if (level == 0) { t->fmt_class = cls; t->w = w; t->h = h; t->has_mips = 0; t->auto_mips = 0; }
  if (level > 0) t->has_mips = 1;
  glTexImage2D(GL_TEXTURE_2D, level, (GLint)ofmt, w, h, 0, ofmt, otype, conv ? conv : px ? GPTR(px) : NULL);
  if (conv) { free(conv); glPixelStorei(GL_UNPACK_ALIGNMENT, unpack); }
  if (level == 0 && px && (w > 1 || h > 1)) { glGenerateMipmap(GL_TEXTURE_2D); t->auto_mips = 1; }
  fix_min_filter(t);
}

HLE(opengl32, glTexImage2D) { /* (target, level, internal, w, h, border, format, type, pixels) */
  tex_image((int)ARG(1), (int)ARG(3), (int)ARG(4), ARG(6), ARG(7), ARG(8), (int)ARG(2));
  RETV(9);
}

HLE(glu32, gluBuild2DMipmaps) { /* (target, internal, w, h, format, type, data) */
  tex_image(0, (int)ARG(2), (int)ARG(3), ARG(4), ARG(5), ARG(6), (int)ARG(1));
  Tex *t = bound_tex();
  if (t) {
    glGenerateMipmap(GL_TEXTURE_2D);
    t->has_mips = 1;
    fix_min_filter(t);
  }
  RET(7, 0);
}

/* ------------------------------------------------------------------ GLU quadrics */
HLE(glu32, gluNewQuadric) { RET(0, sys_alloc(16)); }
HLE(glu32, gluDeleteQuadric) { RETV(1); }
HLE(glu32, gluSphere) { /* (quad, double radius, slices, stacks) */
  double r = ARGD(1);
  int slices = (int)ARG(3), stacks = (int)ARG(4);
  if (slices < 3) slices = 3;
  if (stacks < 2) stacks = 2;
  float saved_n[3];
  memcpy(saved_n, S.cur_normal, 12);
  for (int i = 0; i < stacks; i++) {
    double t0 = M_PI * i / stacks, t1 = M_PI * (i + 1) / stacks;
    imm_mode = GL_QUAD_STRIP;
    imm_n = 0;
    for (int j = 0; j <= slices; j++) {
      double p = 2 * M_PI * j / slices;
      for (int k = 0; k < 2; k++) {
        double t = k ? t1 : t0;
        float x = (float)(sin(t) * sin(p)), y = (float)(sin(t) * cos(p)), z = (float)cos(t);
        S.cur_normal[0] = x; S.cur_normal[1] = y; S.cur_normal[2] = z;
        imm_vertex((float)(x * r), (float)(y * r), (float)(z * r), 1);
      }
    }
    imm_flush();
  }
  memcpy(S.cur_normal, saved_n, 12);
  RETV(5);
}

/* ------------------------------------------------------------------ init */
void gl_init(void) {
  str_vendor = sys_strdup("BSLibs Web Port");
  str_renderer = sys_strdup("WebGL2 fixed-function emulation");
  str_version = sys_strdup("1.3.0");
  str_ext = sys_strdup("GL_ARB_multitexture GL_EXT_bgra GL_EXT_texture_env_add");
  reset_state();
}
