/* Guest heap: a private dlmalloc mspace over the guest heap range of linear memory. */
#define ONLY_MSPACES 1
#define MSPACES 1
#define HAVE_MMAP 0
#define HAVE_MORECORE 0
#define USE_LOCKS 0
#define DLMALLOC_EXPORT static
#define NO_MALLOC_STATS 1
#define MALLOC_FAILURE_ACTION
#define USE_DL_PREFIX 1
#define sys_alloc dl_sys_alloc /* dlmalloc internal; the runtime has its own sys_alloc */
#include "dlmalloc.c" /* emscripten/system/lib, compiled privately as an mspace-only allocator */
#undef sys_alloc
#include "hle.h"

static mspace g_ms;

void gheap_init(void) {
  g_ms = create_mspace_with_base(GPTR(GUEST_HEAP_LO), GUEST_HEAP_HI - GUEST_HEAP_LO, 0);
  if (!g_ms) fatal("create_mspace_with_base failed");
  mspace_set_footprint_limit(g_ms, GUEST_HEAP_HI - GUEST_HEAP_LO);
}

uint32_t gmalloc(uint32_t size) { void *p = mspace_malloc(g_ms, size ? size : 1); return p ? HADDR(p) : 0; }
uint32_t gcalloc(uint32_t size) { void *p = mspace_calloc(g_ms, 1, size ? size : 1); return p ? HADDR(p) : 0; }
uint32_t gmemalign(uint32_t align, uint32_t size) { void *p = mspace_memalign(g_ms, align, size ? size : 1); return p ? HADDR(p) : 0; }
void gfree(uint32_t p) { if (p) mspace_free(g_ms, GPTR(p)); }
uint32_t grealloc(uint32_t p, uint32_t size) {
  if (!p) return gmalloc(size);
  void *r = mspace_realloc(g_ms, GPTR(p), size ? size : 1);
  return r ? HADDR(r) : 0;
}
uint32_t gusable(uint32_t p) { return p ? (uint32_t)mspace_usable_size(GPTR(p)) : 0; }
uint32_t gstrdup(const char *s) {
  uint32_t n = (uint32_t)strlen(s) + 1;
  uint32_t p = gmalloc(n);
  memcpy(GPTR(p), s, n);
  return p;
}

