/*
 * MP4 Player 9300 - common definitions for the portable decoder core.
 *
 * The core (MP4 demuxer, MPEG-4 Part 2 / H.263 decoder, IDCT, colour conversion) is plain
 * C-style C++ that builds with the Symbian GCC 2.9 (EKA1) as well as a modern host compiler,
 * so it can be tested on Linux against ffmpeg.
 *
 * Rules for EKA1 DLLs (a Series 80 .app is a DLL): no writable static data. All tables are
 * "static const" and contain no pointers; everything mutable lives in heap-allocated contexts.
 */
#ifndef MPCOMMON_H
#define MPCOMMON_H

typedef unsigned char  u8;
typedef signed char    s8;
typedef unsigned short u16;
typedef signed short   s16;
typedef unsigned int   u32;
typedef signed int     s32;

/* Allocator supplied by the platform layer (the Symbian app uses its own chunk heap). */
struct MpAlloc
    {
    void* (*alloc)(void* opaque, int size);
    void  (*free)(void* opaque, void* ptr);
    void* opaque;
    };

/* File access supplied by the platform layer. */
struct MpIo
    {
    /* reads up to len bytes at offset; returns the number of bytes read, <0 on error */
    int  (*read)(void* opaque, u32 offset, void* buf, int len);
    void* opaque;
    u32  size;
    };

void* mp_alloc(const MpAlloc* a, int size);         /* zero-filled, NULL on failure */
void  mp_free(const MpAlloc* a, void* p);
void  mp_memset(void* p, int v, int n);
void  mp_memcpy(void* d, const void* s, int n);

static inline int mp_clip(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }
static inline int mp_abs(int v) { return v < 0 ? -v : v; }

/* Bytes of zero padding the decoder needs after every input buffer. */
#define MP_INPUT_PADDING 16

#endif
