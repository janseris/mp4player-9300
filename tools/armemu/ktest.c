/* Kernel tests for the ARM assembler routines (core/h264/h264bsd_arm.c): each routine runs on
 * random data next to a C reference (same arithmetic as the h264bsd C code); the return value
 * is the number of differences. Built into the emulator image only (build.sh), run by
 * run.py ktest. */
#include "../../core/h264/basetype.h"
#include "../../core/h264/h264bsd_arm.h"

#ifndef H264_ARM_ASM
#error ktest is for the ARM build
#endif

extern const u8 h264bsdClip[];
static u32 rnd(u32 *s) { *s = *s * 1103515245u + 12345u; return *s >> 8; }

#define ABSD(a, b) ((a) > (b) ? (a) - (b) : (b) - (a))
#define CLIP3(lo, hi, v) ((v) < (lo) ? (lo) : (v) > (hi) ? (hi) : (v))
static u8 clip255(i32 v) { return (u8)(v < 0 ? 0 : v > 255 ? 255 : v); }

static void ref_luma(u8 *d, i32 a, i32 along, u32 n, const u32 *prm, u32 bS)
{
    u32 alpha = prm[0], beta = prm[1];
    for (; n; n--, d += along)
    {
        i32 p1 = d[-2*a], p0 = d[-a], q0 = d[0], q1 = d[a];
        i32 p2, q2, tc, tmp, val, delta;
        if (!((u32)ABSD(p0, q0) < alpha && (u32)ABSD(p1, p0) < beta && (u32)ABSD(q1, q0) < beta))
            continue;
        p2 = d[-3*a]; q2 = d[2*a];
        if (bS < 4)
        {
            tc = (i32)prm[2]; tmp = tc;
            if ((u32)ABSD(p2, p0) < beta)
            {
                val = (p2 + ((p0 + q0 + 1) >> 1) - (p1 << 1)) >> 1;
                d[-2*a] = (u8)(p1 + CLIP3(-tc, tc, val));
                tmp++;
            }
            if ((u32)ABSD(q2, q0) < beta)
            {
                val = (q2 + ((p0 + q0 + 1) >> 1) - (q1 << 1)) >> 1;
                d[a] = (u8)(q1 + CLIP3(-tc, tc, val));
                tmp++;
            }
            val = (((q0 - p0) << 2) + (p1 - q1) + 4) >> 3;
            delta = CLIP3(-tmp, tmp, val);
            d[-a] = clip255(p0 + delta);
            d[0] = clip255(q0 - delta);
        }
        else
        {
            u32 flag = (u32)ABSD(p0, q0) < ((alpha >> 2) + 2);
            if (flag && (u32)ABSD(p2, p0) < beta)
            {
                tmp = p1 + p0 + q0;
                d[-a] = (u8)((p2 + 2 * tmp + q1 + 4) >> 3);
                d[-2*a] = (u8)((p2 + tmp + 2) >> 2);
                d[-3*a] = (u8)((2 * d[-4*a] + 3 * p2 + tmp + 4) >> 3);
            }
            else
                d[-a] = (u8)((2 * p1 + p0 + q1 + 2) >> 2);
            if (flag && (u32)ABSD(q2, q0) < beta)
            {
                tmp = p0 + q0 + q1;
                d[0] = (u8)((p1 + 2 * tmp + q2 + 4) >> 3);
                d[a] = (u8)((tmp + q2 + 2) >> 2);
                d[2*a] = (u8)((2 * d[3*a] + 3 * q2 + tmp + 4) >> 3);
            }
            else
                d[0] = (u8)((2 * q1 + q0 + p1 + 2) >> 2);
        }
    }
}

static void ref_chroma(u8 *d, i32 a, i32 along, u32 n, const u32 *prm, u32 bS)
{
    for (; n; n--, d += along)
    {
        i32 p1 = d[-2*a], p0 = d[-a], q0 = d[0], q1 = d[a], tc, delta;
        if (!((u32)ABSD(p0, q0) < prm[0] && (u32)ABSD(p1, p0) < prm[1] && (u32)ABSD(q1, q0) < prm[1]))
            continue;
        if (bS < 4)
        {
            tc = (i32)prm[2];
            delta = CLIP3(-tc, tc, ((((q0 - p0) << 2) + (p1 - q1) + 4) >> 3));
            d[-a] = clip255(p0 + delta);
            d[0] = clip255(q0 - delta);
        }
        else
        {
            d[-a] = (u8)((2 * p1 + p0 + q1 + 2) >> 2);
            d[0] = (u8)((2 * q1 + q0 + p1 + 2) >> 2);
        }
    }
}

#define W 48

u32 ktest_deblock(u8 *bufA, u8 *bufB, u32 seed, u32 iters)
{
    u32 it, i, bad = 0;
    u32 prm[3];
    for (it = 0; it < iters; it++)
    {
        u32 kind = rnd(&seed) % 4, bS = 1 + rnd(&seed) % 4, horiz = rnd(&seed) & 1;
        u32 base = rnd(&seed) & 255, spread = 1 + (rnd(&seed) % 64), n = 1 + rnd(&seed) % 16;
        i32 across = horiz ? W : 1, along = horiz ? 1 : W;
        u8 *pa, *pb;
        for (i = 0; i < W * W; i++)
        {
            i32 v = (i32)base + (i32)(rnd(&seed) % (2 * spread + 1)) - (i32)spread;
            if (rnd(&seed) % 23 == 0) v = rnd(&seed) & 255;
            bufA[i] = bufB[i] = clip255(v);
        }
        prm[0] = rnd(&seed) % 256;
        prm[1] = rnd(&seed) % 20;
        prm[2] = rnd(&seed) % 26;
        pa = bufA + 8 * W + 8;
        pb = bufB + 8 * W + 8;
        if (kind < 2)
        {
            if (bS < 4) h264arm_luma_lt4(pb, across, along, n, prm);
            else h264arm_luma_eq4(pb, across, along, n, prm);
            ref_luma(pa, across, along, n, prm, bS);
        }
        else
        {
            if (bS < 4) h264arm_chroma_lt4(pb, across, along, n, prm);
            else h264arm_chroma_eq4(pb, across, along, n, prm);
            ref_chroma(pa, across, along, n, prm, bS);
        }
        for (i = 0; i < W * W; i++) if (bufA[i] != bufB[i]) { bad++; break; }
    }
    return bad;
}

u32 ktest_copy(u8 *bufA, u8 *bufB, u32 seed, u32 iters)
{
    u32 it, i, x, y, bad = 0;
    for (it = 0; it < iters; it++)
    {
        u32 w = 4 << (rnd(&seed) % 3), h = 1 + rnd(&seed) % 16;
        u32 sstride = 20 + rnd(&seed) % 40, dstride = 16 + 4 * (rnd(&seed) % 4);
        u32 soff = rnd(&seed) % 64, doff = 4 * (rnd(&seed) % 8);
        u8 *src = bufA + 2048;
        for (i = 0; i < 2048; i++) { src[i] = (u8)rnd(&seed); bufA[i] = bufB[i] = (u8)rnd(&seed); }
        h264arm_copy_block(src + soff, bufB + doff, sstride, w, h, dstride);
        for (y = 0; y < h; y++)
            for (x = 0; x < w; x++)
                bufA[doff + y * dstride + x] = src[soff + y * sstride + x];
        for (i = 0; i < 2048; i++) if (bufA[i] != bufB[i]) { bad++; break; }
    }
    return bad;
}

static u8 c6(i32 a, i32 b, i32 c, i32 d, i32 e, i32 f)
{
    return clip255((a - 5 * b + 20 * c + 20 * d - 5 * e + f + 16) >> 5);
}

/* hor6 / ver6 against the formula, all modes and sizes */
u32 ktest_interp6(u8 *bufA, u8 *bufB, u32 seed, u32 iters)
{
    u32 it, i, x, y, bad = 0;
    for (it = 0; it < iters; it++)
    {
        u32 w = 4 << (rnd(&seed) % 3), h = 4 << (rnd(&seed) % 3), mode = rnd(&seed) % 4;
        u32 ver = rnd(&seed) & 1, stride = 24 + rnd(&seed) % 40;
        u8 *src = bufA + 4096, *s0;
        u32 kind = rnd(&seed) % 3;
        for (i = 0; i < 4096; i++)
        {
            u32 v = rnd(&seed) & 255;
            if (kind == 1) v = (v & 1) ? 255 : 0;            /* extremes: clipping */
            if (kind == 2) v = 100 + (v & 15);
            src[i] = (u8)v;
        }
        for (i = 0; i < 256; i++) bufA[i] = bufB[i] = (u8)rnd(&seed);
        s0 = src + 3 * stride + 3 + rnd(&seed) % 8;
        if (ver) h264arm_ver6(s0, bufB, (i32)stride, w | h << 8 | mode << 16, h264arm_clip6tbl + 160);
        else h264arm_hor6(s0, bufB, (i32)stride, w | h << 8 | mode << 16, h264arm_clip6tbl + 160);
        for (y = 0; y < h; y++)
            for (x = 0; x < w; x++)
            {
                const u8 *p = s0 + y * stride + x;
                i32 v, g;
                if (ver)
                {
                    v = c6(p[0], p[stride], p[2*stride], p[3*stride], p[4*stride], p[5*stride]);
                    g = mode == 1 ? p[2*stride] : p[3*stride];
                }
                else
                {
                    v = c6(p[0], p[1], p[2], p[3], p[4], p[5]);
                    g = mode == 1 ? p[2] : p[3];
                }
                if (mode == 3) g = bufA[y * 16 + x];
                if (mode) v = (v + g + 1) >> 1;
                bufA[y * 16 + x] = (u8)v;
            }
        for (i = 0; i < 256; i++) if (bufA[i] != bufB[i]) { bad++; break; }
    }
    return bad;
}

/* chroma hor / ver / hv against h264bsd's formulas */
u32 ktest_chroma(u8 *bufA, u8 *bufB, u32 seed, u32 iters)
{
    u32 it, i, x, y, bad = 0;
    for (it = 0; it < iters; it++)
    {
        u32 w = 2 << (rnd(&seed) % 3), h = 2 << (rnd(&seed) % 3);
        u32 xf = rnd(&seed) % 8, yf = rnd(&seed) % 8, kind = rnd(&seed) % 3;
        u32 stride = 12 + rnd(&seed) % 40;
        u8 *src = bufA + 4096, *s0;
        for (i = 0; i < 2048; i++) src[i] = (u8)rnd(&seed);
        for (i = 0; i < 64; i++) bufA[i] = bufB[i] = (u8)rnd(&seed);
        s0 = src + stride + rnd(&seed) % 8;
        if (kind == 0) h264arm_chroma_hor(s0, bufB, (i32)stride, w | h << 8 | xf << 16);
        else if (kind == 1) h264arm_chroma_ver(s0, bufB, (i32)stride, w | h << 8 | yf << 16);
        else h264arm_chroma_hv(s0, bufB, (i32)stride, w | h << 8 | xf << 16 | yf << 24);
        for (y = 0; y < h; y++)
            for (x = 0; x < w; x++)
            {
                const u8 *p = s0 + y * stride + x;
                u32 c;
                if (kind == 0) c = ((((8 - xf) * p[0] + xf * p[1]) << 3) + 32) >> 6;
                else if (kind == 1) c = ((((8 - yf) * p[0] + yf * p[stride]) << 3) + 32) >> 6;
                else
                {
                    u32 l = p[0] * (8 - yf) + p[stride] * yf, r = p[1] * (8 - yf) + p[stride + 1] * yf;
                    c = (l * (8 - xf) + 32 + r * xf) >> 6;
                }
                bufA[y * 8 + x] = (u8)c;
            }
        for (i = 0; i < 64; i++) if (bufA[i] != bufB[i]) { bad++; break; }
    }
    return bad;
}

/* idct4x4 against h264bsdProcessBlock's C arithmetic (general case) */
static u32 ref_idct(i32 *data, const i32 *sc)
{
    static const u8 src[16] = { 0, 1, 5, 6, 2, 4, 7, 12, 3, 8, 11, 13, 9, 10, 14, 15 };
    static const u8 sci[16] = { 3, 1, 0, 1, 1, 2, 1, 2, 0, 1, 0, 1, 1, 2, 1, 2 };
    i32 in[16], t0, t1, t2, t3;
    u32 k, r, c;
    for (k = 0; k < 16; k++) in[k] = data[k];
    for (k = 0; k < 16; k++) data[k] = sci[k] == 3 ? in[src[k]] : in[src[k]] * sc[sci[k]];
    for (r = 0; r < 4; r++)
    {
        i32 *p = data + 4 * r;
        t0 = p[0] + p[2]; t1 = p[0] - p[2]; t2 = (p[1] >> 1) - p[3]; t3 = p[1] + (p[3] >> 1);
        p[0] = t0 + t3; p[1] = t1 + t2; p[2] = t1 - t2; p[3] = t0 - t3;
    }
    for (c = 0; c < 4; c++)
    {
        i32 *p = data + c;
        t0 = p[0] + p[8]; t1 = p[0] - p[8]; t2 = (p[4] >> 1) - p[12]; t3 = p[4] + (p[12] >> 1);
        p[0] = (t0 + t3 + 32) >> 6; p[4] = (t1 + t2 + 32) >> 6;
        p[8] = (t1 - t2 + 32) >> 6; p[12] = (t0 - t3 + 32) >> 6;
        if ((u32)(p[0] + 512) > 1023 || (u32)(p[4] + 512) > 1023 ||
            (u32)(p[8] + 512) > 1023 || (u32)(p[12] + 512) > 1023)
            return 1;
    }
    return 0;
}

u32 ktest_idct(u8 *bufA, u8 *bufB, u32 seed, u32 iters)
{
    u32 it, i, bad = 0;
    i32 *a = (i32 *)bufA, *b = (i32 *)bufB, sc[3];
    for (it = 0; it < iters; it++)
    {
        u32 big = rnd(&seed) % 4, ra, rb;
        u32 q = rnd(&seed) % 52;
        sc[0] = (10 + rnd(&seed) % 16) << (q / 6);
        sc[1] = (13 + rnd(&seed) % 16) << (q / 6);
        sc[2] = (16 + rnd(&seed) % 16) << (q / 6);
        for (i = 0; i < 16; i++)
        {
            i32 v = (i32)(rnd(&seed) % 9) - 4;
            if (big == 0) v = (i32)(rnd(&seed) % 401) - 200;      /* often out of range */
            if (big == 1 && rnd(&seed) % 3) v = 0;
            a[i] = b[i] = v;
        }
        a[0] = b[0] = a[0] * sc[0];
        ra = ref_idct(a, sc);
        rb = h264arm_idct4x4(b, sc);
        if (ra != rb) { bad++; continue; }
        if (ra) continue;       /* out of range: the block is an error, data doesn't matter */
        for (i = 0; i < 16; i++) if (a[i] != b[i]) { bad++; break; }
    }
    return bad;
}

/* write_mb + add4x4 */
u32 ktest_write(u8 *bufA, u8 *bufB, u32 seed, u32 iters)
{
    u32 it, i, x, y, bad = 0;
    const u8 *clip = h264bsdClip + 512;
    for (it = 0; it < iters; it++)
    {
        u32 LW = 32 + 16 * (rnd(&seed) % 3);          /* luma line length */
        u8 *data = bufA + 8192, *pa = bufA, *pb = bufB;
        i32 *res = (i32 *)(bufA + 12288);
        for (i = 0; i < 384; i++) data[i] = (u8)rnd(&seed);
        for (i = 0; i < 16; i++) res[i] = (i32)(rnd(&seed) % 1024) - 512;
        for (i = 0; i < 4096; i++) pa[i] = pb[i] = (u8)rnd(&seed);
        h264arm_write_mb(pb, pb + 2048, pb + 3072, data, LW);
        h264arm_add4x4(pb + 4 * LW + 4, res, LW, clip);
        for (y = 0; y < 16; y++) for (x = 0; x < 16; x++) pa[y * LW + x] = data[y * 16 + x];
        for (y = 0; y < 8; y++) for (x = 0; x < 8; x++)
        {
            pa[2048 + y * (LW / 2) + x] = data[256 + y * 8 + x];
            pa[3072 + y * (LW / 2) + x] = data[320 + y * 8 + x];
        }
        for (y = 0; y < 4; y++) for (x = 0; x < 4; x++)
        {
            i32 v = pa[(4 + y) * LW + 4 + x] + res[y * 4 + x];
            pa[(4 + y) * LW + 4 + x] = (u8)(v < 0 ? 0 : v > 255 ? 255 : v);
        }
        for (i = 0; i < 4096; i++) if (pa[i] != pb[i]) { bad++; break; }
    }
    return bad;
}
