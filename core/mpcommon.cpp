#include "mpcommon.h"

#ifdef __SYMBIAN32__
#include <e32std.h>
void mp_memset(void* p, int v, int n) { if (n > 0) Mem::Fill(p, n, TChar(v & 0xFF)); }
void mp_memcpy(void* d, const void* s, int n) { if (n > 0) Mem::Copy(d, s, n); }
#else
#include <string.h>
void mp_memset(void* p, int v, int n) { if (n > 0) memset(p, v, n); }
void mp_memcpy(void* d, const void* s, int n) { if (n > 0) memcpy(d, s, n); }
#endif

void* mp_alloc(const MpAlloc* a, int size)
    {
    void* p;
    if (size <= 0) size = 4;
    p = a->alloc(a->opaque, size);
    if (p) mp_memset(p, 0, size);
    return p;
    }

void mp_free(const MpAlloc* a, void* p)
    {
    if (p) a->free(a->opaque, p);
    }
