/* Win32 layer start-up and export lookup (GetProcAddress). */
#include "hle.h"
#include "win32.h"
#include <strings.h>

extern const struct { uint32_t slot; const char *dll; const char *name; gfn f; } g_imports[];
void dinput_init(void);

uint32_t hle_lookup(const char *module, const char *name) {
  size_t ml = strlen(module);
  for (int k = 0; g_imports[k].slot; k++) {
    const char *d = g_imports[k].dll;
    if (strncasecmp(d, module, ml) || (d[ml] && d[ml] != '.')) continue;
    const char *n = g_imports[k].name;
    /* fmod exports are decorated (_FSOUND_Init@12) */
    if (!strcmp(n, name) || (n[0] == '_' && !strncmp(n + 1, name, strlen(name)) && n[1 + strlen(name)] == '@'))
      return LD32(g_imports[k].slot);
  }
  return 0;
}

void win32_init(void) {
  kernel32_init();
  user32_init();
  gl_init();
  dinput_init();
}
