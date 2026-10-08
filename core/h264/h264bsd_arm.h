/*
 * MP4 Player 9300: hand-written ARM (ARMv4T, no ARMv5E/v6 instructions) versions of the hot
 * loops of h264bsd, for the Nokia 9300's 150 MHz ARM9. Used when compiling for ARM with GCC;
 * every other build uses the original C code. The results are bit-identical to the C code.
 */
#ifndef H264BSD_ARM_H
#define H264BSD_ARM_H

#include "basetype.h"

#if defined(__GNUC__) && defined(__arm__) && !defined(H264_NO_ASM)
#define H264_ARM_ASM 1
#endif

#ifdef H264_ARM_ASM
#ifdef __cplusplus
extern "C" {
#endif

/* Deblocking. 'across' is the step from one side of the edge to the other (1 for a vertical
 * edge, the line length for a horizontal one), 'along' the step to the next pixel of the edge,
 * n the number of pixels. prm = { alpha, beta, tc } (tc only for bS < 4: tc0 for luma,
 * tc0 + 1 for chroma). */
void h264arm_luma_lt4(u8 *data, i32 across, i32 along, u32 n, const u32 *prm);
void h264arm_luma_eq4(u8 *data, i32 across, i32 along, u32 n, const u32 *prm);
void h264arm_chroma_lt4(u8 *data, i32 across, i32 along, u32 n, const u32 *prm);
void h264arm_chroma_eq4(u8 *data, i32 across, i32 along, u32 n, const u32 *prm);

/* Motion compensation, full-pel: copies a w x h block (w = 4, 8 or 16; dst and dstStride
 * multiples of 4, src any alignment). */
void h264arm_copy_block(const u8 *src, u8 *dst, u32 srcStride, u32 w, u32 h, u32 dstStride);

/* Luma 6-tap interpolation of a w x h block (w, h = 4, 8 or 16) into the 16-wide prediction
 * buffer: whm = w | h << 8 | mode << 16; tbl = h264arm_clip6tbl + 160.
 * hor6: src 2 pixels left of the block; mode 0 b, 1 a (average with the pixel on the left),
 * 2 c (on the right), 3 average with dst. ver6: src 2 lines above; mode 0 h, 1 d, 2 n, 3 dst. */
extern const u8 h264arm_clip6tbl[830];
void h264arm_hor6(const u8 *src, u8 *dst, i32 srcStride, u32 whm, const u8 *tbl);
void h264arm_ver6(const u8 *src, u8 *dst, i32 srcStride, u32 whm, const u8 *tbl);

/* Chroma interpolation of a w x h block (w, h = 2, 4 or 8) into the 8-wide prediction buffer:
 * whf = w | h << 8 | frac << 16 (hor: xFrac, ver: yFrac); hv: whxy = w | h << 8 | xFrac << 16 |
 * yFrac << 24. src points at the block's top-left reference pixel. */
void h264arm_chroma_hor(const u8 *src, u8 *dst, i32 stride, u32 whf);
void h264arm_chroma_ver(const u8 *src, u8 *dst, i32 stride, u32 whf);
void h264arm_chroma_hv(const u8 *src, u8 *dst, i32 stride, u32 whxy);

/* h264bsdProcessBlock's general case (zig-zag, dequantisation with scale = { t1, t2, t3 },
 * inverse transform); returns 1 if a result is out of range */
u32 h264arm_idct4x4(i32 *data, const i32 *scale);
/* predicted macroblock (16x16 luma, 8x8 cb, 8x8 cr) into the picture */
void h264arm_write_mb(u8 *lum, u8 *cb, u8 *cr, const u8 *data, u32 width);
/* dst = clip(dst + res), 4x4, dst word aligned; clip = h264bsdClip + 512 */
void h264arm_add4x4(u8 *dst, const i32 *res, u32 stride, const u8 *clip);

#ifdef __cplusplus
}
#endif
#endif

#endif
