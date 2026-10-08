#include "yuv2rgb.h"

#define CLIP_OFS 384
#define CLIP_N   1024
#define KT_OFS   256        /* index range of Y + chroma offset: -256 .. 767 */
#define KT_N     1024

struct YuvConv
    {
    MpAlloc al;
    int srcW, srcH, dstW, dstH, format;
    int cx, cy;             /* crop window origin; srcW/srcH are the window size */
    int crop;
    int yTab[256];          /* 1.164 * (Y - 16), in pixel units * 1 (rounded) */
    int rv[256], gu[256], gv[256], bu[256];
    u16 rTab[CLIP_N], gTab[CLIP_N], bTab[CLIP_N];   /* clipped, shifted into place (16-bit formats) */
    u8 clip[CLIP_N];
    /* 1:1 fast path, written for GCC 2.9 (which has no 16-bit loads: only ints and bytes here).
       R = clip(1.164 * (Y + vr[V] - 16)) etc.: the chroma terms are in luma units, so each colour
       is one byte-table lookup at [Y + offset]. The byte tables hold the value already masked for
       the pixel format; the shift into place comes free with the ARM's ORR. */
    int vT[512];            /* [2v] = V's red offset, [2v+1] = V's green offset (both in luma units) */
    int uT[512];            /* [2u] = U's green offset, [2u+1] = U's blue offset */
    u8 kT[3 * KT_N];        /* red, green, blue byte tables (masked for the format), back to back */
    int fastFormat;
    u16* xmap;              /* dstW source x coordinates */
    int* xmapI;             /* the same as ints (GCC 2.9 has no 16-bit loads) */
    u16* ymap;              /* dstH source y coordinates */
    int xmapLen, ymapLen;
    };

int yuvconv_open(YuvConv** aConv, const MpAlloc* al)
    {
    YuvConv* c;
    int i;
    *aConv = 0;
    c = (YuvConv*)mp_alloc(al, sizeof(YuvConv));
    if (!c) return -1;
    c->al = *al;
    for (i = 0; i < 256; i++)
        {
        /* fixed point with 16 fractional bits, rounded to pixel units */
        c->yTab[i] = (76309 * (i - 16) + 32768) >> 16;
        c->rv[i] = (104597 * (i - 128) + 32768) >> 16;
        c->gu[i] = (25675 * (i - 128) + 32768) >> 16;
        c->gv[i] = (53279 * (i - 128) + 32768) >> 16;
        c->bu[i] = (132201 * (i - 128) + 32768) >> 16;
        /* 1.596/1.164, 0.391/1.164, 0.813/1.164, 2.018/1.164 in 16.16 fixed point */
        c->vT[2 * i] = (89858 * (i - 128) + 32768) >> 16;
        c->vT[2 * i + 1] = -((45773 * (i - 128) + 32768) >> 16);
        c->uT[2 * i] = -((22058 * (i - 128) + 32768) >> 16);
        c->uT[2 * i + 1] = (113575 * (i - 128) + 32768) >> 16;
        }
    c->fastFormat = -1;
    for (i = 0; i < CLIP_N; i++)
        {
        int v = i - CLIP_OFS;
        if (v < 0) v = 0;
        if (v > 255) v = 255;
        c->clip[i] = (u8)v;
        }
    c->format = -1;
    *aConv = c;
    return 0;
    }

void yuvconv_close(YuvConv* c)
    {
    MpAlloc al;
    if (!c) return;
    al = c->al;
    mp_free(&al, c->xmap);
    mp_free(&al, c->xmapI);
    mp_free(&al, c->ymap);
    mp_free(&al, c);
    }

int yuvconv_setup(YuvConv* c, int srcW, int srcH, int dstW, int dstH, int format)
    {
    return yuvconv_setup_crop(c, 0, 0, srcW, srcH, dstW, dstH, format);
    }

int yuvconv_setup_crop(YuvConv* c, int cx, int cy, int srcW, int srcH, int dstW, int dstH, int format)
    {
    int i;
    if (dstW <= 0 || dstH <= 0 || srcW <= 0 || srcH <= 0 || cx < 0 || cy < 0) return -1;
    if (c->format == format && c->cx == cx && c->cy == cy && c->srcW == srcW && c->srcH == srcH &&
        c->dstW == dstW && c->dstH == dstH)
        return 0;                               /* unchanged */
    if (dstW > c->xmapLen)
        {
        mp_free(&c->al, c->xmap);
        mp_free(&c->al, c->xmapI);
        c->xmap = (u16*)mp_alloc(&c->al, dstW * 2);
        c->xmapI = (int*)mp_alloc(&c->al, dstW * 4);
        if (!c->xmap || !c->xmapI) { c->xmapLen = 0; return -1; }
        c->xmapLen = dstW;
        }
    if (dstH > c->ymapLen)
        {
        mp_free(&c->al, c->ymap);
        c->ymap = (u16*)mp_alloc(&c->al, dstH * 2);
        if (!c->ymap) { c->ymapLen = 0; return -1; }
        c->ymapLen = dstH;
        }
    for (i = 0; i < dstW; i++)
        {
        c->xmap[i] = (u16)(cx + ((2 * i + 1) * srcW) / (2 * dstW));
        c->xmapI[i] = c->xmap[i];
        }
    for (i = 0; i < dstH; i++) c->ymap[i] = (u16)(cy + ((2 * i + 1) * srcH) / (2 * dstH));
    if (format != c->format)
        {
        for (i = 0; i < CLIP_N; i++)
            {
            int v = c->clip[i];
            if (format == YUV_RGB444)
                {
                c->rTab[i] = (u16)((v >> 4) << 8);
                c->gTab[i] = (u16)((v >> 4) << 4);
                c->bTab[i] = (u16)(v >> 4);
                }
            else
                {
                c->rTab[i] = (u16)((v >> 3) << 11);
                c->gTab[i] = (u16)((v >> 2) << 5);
                c->bTab[i] = (u16)(v >> 3);
                }
            }
        }
    c->srcW = srcW; c->srcH = srcH; c->dstW = dstW; c->dstH = dstH; c->format = format;
    c->cx = cx; c->cy = cy;
    c->crop = (cx != 0 || cy != 0);
    return 0;
    }

static void fast_tables(YuvConv* c)
    {
    int k, is565 = (c->format == YUV_RGB565);
    for (k = 0; k < KT_N; k++)
        {
        int v = ((k - KT_OFS - 16) * 76309 + 32768) >> 16;
        if (v < 0) v = 0;
        if (v > 255) v = 255;
        c->kT[k] = (u8)(v & (is565 ? 0xF8 : 0xF0));
        c->kT[KT_N + k] = (u8)(v & (is565 ? 0xFC : 0xF0));
        c->kT[2 * KT_N + k] = (u8)v;
        }
    c->fastFormat = c->format;
    }

/* One row, two pixels (one chroma sample) per step, one 32-bit store per step. Few live pointers
   so that GCC 2.9 keeps everything in registers. */
static void row565(const u8* yr, const u8* ur, const u8* vr, u32* d, u32* end, const int* uT, const int* vT, const u8* k)
    {
    while (d < end)
        {
        const int* vv = vT + 2 * *vr++;
        const int* uu = uT + 2 * *ur++;
        const u8* rp = k + vv[0];
        const u8* gp = k + KT_N + vv[1] + uu[0];
        const u8* bp = k + 2 * KT_N + uu[1];
        int y0 = yr[0], y1 = yr[1];
        yr += 2;
        *d++ = ((u32)rp[y0] << 8) | ((u32)gp[y0] << 3) | ((u32)bp[y0] >> 3) |
               ((u32)rp[y1] << 24) | ((u32)gp[y1] << 19) | (((u32)bp[y1] >> 3) << 16);
        }
    }

static void row444(const u8* yr, const u8* ur, const u8* vr, u32* d, u32* end, const int* uT, const int* vT, const u8* k)
    {
    while (d < end)
        {
        const int* vv = vT + 2 * *vr++;
        const int* uu = uT + 2 * *ur++;
        const u8* rp = k + vv[0];
        const u8* gp = k + KT_N + vv[1] + uu[0];
        const u8* bp = k + 2 * KT_N + uu[1];
        int y0 = yr[0], y1 = yr[1];
        yr += 2;
        *d++ = ((u32)rp[y0] << 4) | (u32)gp[y0] | ((u32)bp[y0] >> 4) |
               ((u32)rp[y1] << 20) | ((u32)gp[y1] << 16) | (((u32)bp[y1] >> 4) << 16);
        }
    }

#if defined(__GNUC__) && defined(__arm__)
/* Two rows of RGB565 sharing one chroma row, hand-written ARM (GCC 2.9 can't keep this loop in
   registers: ~12 instructions per pixel here against 22 for the C version). Same tables, same
   output as row565(). Table layout: vT at base, uT at base + 2048, kT at base + 4096 (KT_OFS 256,
   so k = base + 4352; green + 1024, blue + 2048). */
struct Rows2Args
    {
    const u8* y0; const u8* y1; const u8* u; const u8* v;
    u32* d0; u32* d1; u32* end0; const int* base;
    };

static void rows565_arm(Rows2Args* a)
    {
    asm volatile(
        "stmfd   sp!, {r0-r12, lr}\n\t"
        "mov     r12, %0\n\t"
        "ldmia   r12, {r0-r7}\n\t"
        "cmp     r4, r6\n\t"
        "bcs     2f\n"
        "1:\n\t"
        "ldrb    r11, [r3], #1\n\t"
        "ldrb    r12, [r2], #1\n\t"
        "add     r11, r7, r11, lsl #3\n\t"
        "add     r12, r7, r12, lsl #3\n\t"
        "ldr     r8, [r11]\n\t"
        "ldr     r9, [r11, #4]\n\t"
        "ldr     r10, [r12, #2052]\n\t"
        "ldr     r11, [r12, #2048]\n\t"
        "add     r8, r8, r7\n\t"
        "add     r8, r8, #0x1100\n\t"
        "add     r9, r9, r11\n\t"
        "add     r9, r9, r7\n\t"
        "add     r9, r9, #0x1500\n\t"
        "add     r10, r10, r7\n\t"
        "add     r10, r10, #0x1900\n\t"
        /* row 0, two pixels */
        "ldrb    lr, [r0], #1\n\t"
        "ldrb    r11, [r8, lr]\n\t"
        "mov     r12, r11, lsl #8\n\t"
        "ldrb    r11, [r9, lr]\n\t"
        "orr     r12, r12, r11, lsl #3\n\t"
        "ldrb    r11, [r10, lr]\n\t"
        "orr     r12, r12, r11, lsr #3\n\t"
        "ldrb    lr, [r0], #1\n\t"
        "ldrb    r11, [r8, lr]\n\t"
        "orr     r12, r12, r11, lsl #24\n\t"
        "ldrb    r11, [r9, lr]\n\t"
        "orr     r12, r12, r11, lsl #19\n\t"
        "ldrb    r11, [r10, lr]\n\t"
        "mov     r11, r11, lsr #3\n\t"
        "orr     r12, r12, r11, lsl #16\n\t"
        "str     r12, [r4], #4\n\t"
        /* row 1, two pixels */
        "ldrb    lr, [r1], #1\n\t"
        "ldrb    r11, [r8, lr]\n\t"
        "mov     r12, r11, lsl #8\n\t"
        "ldrb    r11, [r9, lr]\n\t"
        "orr     r12, r12, r11, lsl #3\n\t"
        "ldrb    r11, [r10, lr]\n\t"
        "orr     r12, r12, r11, lsr #3\n\t"
        "ldrb    lr, [r1], #1\n\t"
        "ldrb    r11, [r8, lr]\n\t"
        "orr     r12, r12, r11, lsl #24\n\t"
        "ldrb    r11, [r9, lr]\n\t"
        "orr     r12, r12, r11, lsl #19\n\t"
        "ldrb    r11, [r10, lr]\n\t"
        "mov     r11, r11, lsr #3\n\t"
        "orr     r12, r12, r11, lsl #16\n\t"
        "str     r12, [r5], #4\n\t"
        "cmp     r4, r6\n\t"
        "bcc     1b\n"
        "2:\n\t"
        "ldmfd   sp!, {r0-r12, lr}\n\t"
        : : "r"(a) : "memory", "cc");
    }
#define HAVE_ROWS565_ARM 1
#endif

static void run_same16(YuvConv* c, const M4vPicture* pic, u8* dst, int dstStride)
    {
    int ofs = c->cx & ~1;
    int oy = 0, pairs = c->dstW >> 1;
    if (c->fastFormat != c->format) fast_tables(c);
#ifdef HAVE_ROWS565_ARM
    if (c->format == YUV_RGB565 && !(c->cy & 1) && !(c->dstW & 1) &&
        (const u8*)c->uT == (const u8*)c->vT + 2048 && c->kT == (const u8*)c->vT + 4096)
        {
        Rows2Args a;
        for (oy = 0; oy < c->dstH; oy += 2)
            {
            int sy = c->cy + oy;
            int two = oy + 1 < c->dstH;
            a.y0 = pic->y + sy * pic->ystride + ofs;
            a.y1 = two ? a.y0 + pic->ystride : a.y0;
            a.u = pic->u + (sy >> 1) * pic->cstride + (ofs >> 1);
            a.v = pic->v + (sy >> 1) * pic->cstride + (ofs >> 1);
            a.d0 = (u32*)(dst + oy * dstStride);
            a.d1 = two ? (u32*)(dst + (oy + 1) * dstStride) : a.d0;
            a.end0 = a.d0 + pairs;
            a.base = c->vT;
            rows565_arm(&a);
            }
        return;
        }
#endif
    for (oy = 0; oy < c->dstH; oy++)
        {
        int sy = c->cy + oy;
        const u8* yr = pic->y + sy * pic->ystride + ofs;
        const u8* ur = pic->u + (sy >> 1) * pic->cstride + (ofs >> 1);
        const u8* vr = pic->v + (sy >> 1) * pic->cstride + (ofs >> 1);
        u32* d = (u32*)(dst + oy * dstStride);
        const u8* k = c->kT + KT_OFS;
        if (c->format == YUV_RGB565) row565(yr, ur, vr, d, d + pairs, c->uT, c->vT, k);
        else row444(yr, ur, vr, d, d + pairs, c->uT, c->vT, k);
        if (c->dstW & 1)
            {
            /* odd width: last pixel through the same tables */
            int x = c->dstW - 1, u = ur[x >> 1], v = vr[x >> 1], y = yr[x];
            int r = k[y + c->vT[2 * v]], g = k[KT_N + y + c->vT[2 * v + 1] + c->uT[2 * u]];
            int b = k[2 * KT_N + y + c->uT[2 * u + 1]];
            ((u16*)d)[x] = (u16)(c->format == YUV_RGB565 ? ((r << 8) | (g << 3) | (b >> 3)) : ((r << 4) | g | (b >> 4)));
            }
        }
    }

/* scaled 16-bit conversion (nearest neighbour), two output pixels per step sharing the chroma
   sample of the first; same tables as the 1:1 path */
static void run_scaled16(YuvConv* c, const M4vPicture* pic, u8* dst, int dstStride)
    {
    const u8* k = c->kT + KT_OFS;
    const int* uT = c->uT;
    const int* vT = c->vT;
    int is565 = (c->format == YUV_RGB565);
    int oy, pairs = c->dstW >> 1;
    if (c->fastFormat != c->format) fast_tables(c);
    for (oy = 0; oy < c->dstH; oy++)
        {
        int sy = c->ymap[oy];
        const u8* yr = pic->y + sy * pic->ystride;
        const u8* ur = pic->u + (sy >> 1) * pic->cstride;
        const u8* vr = pic->v + (sy >> 1) * pic->cstride;
        u32* d = (u32*)(dst + oy * dstStride);
        u32* end = d + pairs;
        const int* xm = c->xmapI;
        while (d < end)
            {
            int x0 = xm[0], x1 = xm[1];
            const int* vv = vT + 2 * vr[x0 >> 1];
            const int* uu = uT + 2 * ur[x0 >> 1];
            const u8* rp = k + vv[0];
            const u8* gp = k + KT_N + vv[1] + uu[0];
            const u8* bp = k + 2 * KT_N + uu[1];
            int y0 = yr[x0], y1 = yr[x1];
            xm += 2;
            if (is565)
                *d++ = ((u32)rp[y0] << 8) | ((u32)gp[y0] << 3) | ((u32)bp[y0] >> 3) |
                       ((u32)rp[y1] << 24) | ((u32)gp[y1] << 19) | (((u32)bp[y1] >> 3) << 16);
            else
                *d++ = ((u32)rp[y0] << 4) | (u32)gp[y0] | ((u32)bp[y0] >> 4) |
                       ((u32)rp[y1] << 20) | ((u32)gp[y1] << 16) | (((u32)bp[y1] >> 4) << 16);
            }
        if (c->dstW & 1)
            {
            int x = c->xmapI[c->dstW - 1], u = ur[x >> 1], v = vr[x >> 1], y = yr[x];
            int r = k[y + vT[2 * v]], g = k[KT_N + y + vT[2 * v + 1] + uT[2 * u]];
            int b = k[2 * KT_N + y + uT[2 * u + 1]];
            ((u16*)d)[0] = (u16)(is565 ? ((r << 8) | (g << 3) | (b >> 3)) : ((r << 4) | g | (b >> 4)));
            }
        }
    }

void yuvconv_run(YuvConv* c, const M4vPicture* pic, u8* dst, int dstStride)
    {
    if ((c->dstW != c->srcW || c->dstH != c->srcH) &&
        (c->format == YUV_RGB565 || c->format == YUV_RGB444) &&
        !(((unsigned long)dst) & 3) && !(dstStride & 3))
        {
        run_scaled16(c, pic, dst, dstStride);
        return;
        }
    if (c->dstW == c->srcW && c->dstH == c->srcH &&
        (c->format == YUV_RGB565 || c->format == YUV_RGB444) &&
        !(((unsigned long)dst) & 3) && !(dstStride & 3))
        {
        run_same16(c, pic, dst, dstStride);
        return;
        }
    {
    int oy, ox;
    const u16* rT = c->rTab + CLIP_OFS;
    const u16* gT = c->gTab + CLIP_OFS;
    const u16* bT = c->bTab + CLIP_OFS;
    const u8* cl = c->clip + CLIP_OFS;
    /* 1:1 (possibly cropped): no per-pixel coordinate table needed */
    int same = (c->dstW == c->srcW && c->dstH == c->srcH);
    int ofs = same ? (c->cx & ~1) : 0;
    for (oy = 0; oy < c->dstH; oy++)
        {
        int sy = same ? c->cy + oy : c->ymap[oy];
        const u8* yr = pic->y + sy * pic->ystride + ofs;
        const u8* ur = pic->u + (sy >> 1) * pic->cstride + (ofs >> 1);
        const u8* vr = pic->v + (sy >> 1) * pic->cstride + (ofs >> 1);
        u8* row = dst + oy * dstStride;
        if (c->format == YUV_RGB565 || c->format == YUV_RGB444)
            {
            u16* d = (u16*)row;
            if (same)
                {
                for (ox = 0; ox + 1 < c->dstW; ox += 2)
                    {
                    int u = ur[ox >> 1], v = vr[ox >> 1];
                    int r = c->rv[v], g = -(c->gu[u] + c->gv[v]), b = c->bu[u];
                    int y0 = c->yTab[yr[ox]], y1 = c->yTab[yr[ox + 1]];
                    d[ox] = (u16)(rT[y0 + r] | gT[y0 + g] | bT[y0 + b]);
                    d[ox + 1] = (u16)(rT[y1 + r] | gT[y1 + g] | bT[y1 + b]);
                    }
                if (ox < c->dstW)
                    {
                    int u = ur[ox >> 1], v = vr[ox >> 1];
                    int y0 = c->yTab[yr[ox]];
                    d[ox] = (u16)(rT[y0 + c->rv[v]] | gT[y0 - c->gu[u] - c->gv[v]] | bT[y0 + c->bu[u]]);
                    }
                }
            else
                {
                const u16* xm = c->xmap;
                for (ox = 0; ox < c->dstW; ox++)
                    {
                    int sx = xm[ox];
                    int u = ur[sx >> 1], v = vr[sx >> 1];
                    int y0 = c->yTab[yr[sx]];
                    d[ox] = (u16)(rT[y0 + c->rv[v]] | gT[y0 - c->gu[u] - c->gv[v]] | bT[y0 + c->bu[u]]);
                    }
                }
            }
        else
            {
            for (ox = 0; ox < c->dstW; ox++)
                {
                int sx = same ? ox : c->xmap[ox];
                int u = ur[sx >> 1], v = vr[sx >> 1];
                int y0 = c->yTab[yr[sx]];
                u8 r = cl[y0 + c->rv[v]], g = cl[y0 - c->gu[u] - c->gv[v]], b = cl[y0 + c->bu[u]];
                if (c->format == YUV_BGR888) { row[3 * ox] = b; row[3 * ox + 1] = g; row[3 * ox + 2] = r; }
                else { row[4 * ox] = b; row[4 * ox + 1] = g; row[4 * ox + 2] = r; row[4 * ox + 3] = 0; }
                }
            }
        }
    }
    }
