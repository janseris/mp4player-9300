// Runtime for running the decoder core, compiled by the phone's GCC 2.9, in the Unicorn ARM emulator
// (tools/armemu). Replaces core/mpcommon.cpp (which uses Symbian's Mem::) and provides a heap.
#include "mpcommon.h"

static u8* gHeapNext;       // writable static data is fine here (not an EKA1 DLL)
static u8* gHeapEnd;

extern "C" void emu_heap_init(u8* base, int size) { gHeapNext = base; gHeapEnd = base + size; }

extern "C" void* emu_alloc(void* /*opaque*/, int size)
    {
    u8* p = gHeapNext;
    size = (size + 7) & ~7;
    if (p + size > gHeapEnd) return 0;
    gHeapNext += size;
    return p;
    }

extern "C" void emu_free(void* /*opaque*/, void* /*p*/) {}

// word-wise copy/fill when aligned, like Symbian's optimised Mem::Copy / Mem::Fill
void mp_memset(void* p, int v, int n)
    {
    u8* d = (u8*)p;
    u32 w = (u32)(v & 255) * 0x01010101u;
    while (n > 0 && ((unsigned long)d & 3)) { *d++ = (u8)v; n--; }
    while (n >= 4) { *(u32*)d = w; d += 4; n -= 4; }
    while (n-- > 0) *d++ = (u8)v;
    }

void mp_memcpy(void* dst, const void* src, int n)
    {
    u8* d = (u8*)dst;
    const u8* s = (const u8*)src;
    if (!(((unsigned long)d | (unsigned long)s) & 3))
        while (n >= 4) { *(u32*)d = *(const u32*)s; d += 4; s += 4; n -= 4; }
    while (n-- > 0) *d++ = *s++;
    }

void* mp_alloc(const MpAlloc* a, int size)
    {
    void* p;
    if (size <= 0) size = 4;
    p = a->alloc(a->opaque, size);
    if (p) mp_memset(p, 0, size);
    return p;
    }

void mp_free(const MpAlloc* a, void* p) { if (p) a->free(a->opaque, p); }

// C library pieces the H.264 decoder (h264bsd, C code) needs; on the phone they come from EUSER
// (memcpy, memset) and core/h264/h264port.cpp (allocation).
extern "C" void* h264port_malloc(unsigned int n) { return emu_alloc(0, (int)n); }
extern "C" void h264port_free(void* /*p*/) {}
extern "C" void* memcpy(void* d, const void* s, unsigned int n) { mp_memcpy(d, s, (int)n); return d; }
extern "C" void* memset(void* d, int v, unsigned int n) { mp_memset(d, v, (int)n); return d; }
