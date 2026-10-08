/*
 * 8x8 integer inverse DCT, 8-bit output ("simple IDCT").
 * Algorithm and constants from FFmpeg's simple_idct (Michael Niedermayer, LGPL-2.1-or-later);
 * using the same IDCT as ffmpeg's default MPEG-4 decoder keeps P-frame drift identical to
 * the reference decoder, so the result can be checked bit-exactly on a PC.
 *
 * The block is int[64] (not short): the phone's GCC 2.9 has no 16-bit loads/stores and turns every
 * short access into byte operations. Rows that are all zero are skipped, a block with only its
 * first row is done with one multiply per column, and the functions leave the block all zero
 * again (only the rows they used are cleared).
 */
#include "idct.h"

#define W1 22725
#define W2 21407
#define W3 19266
#define W4 16383
#define W5 12873
#define W6 8867
#define W7 4520
#define ROW_SHIFT 11
#define COL_SHIFT 20
#define DC_SHIFT 3

static inline u8 clip8(int v) { return (u8)((v & ~255) ? ((-v) >> 31) & 255 : v); }

static void idct_row(int* row)
    {
    int a0, a1, a2, a3, b0, b1, b2, b3;
    if (!(row[1] | row[2] | row[3] | row[4] | row[5] | row[6] | row[7]))
        {
        int v = (s16)(row[0] * (1 << DC_SHIFT));
        row[0] = row[1] = row[2] = row[3] = row[4] = row[5] = row[6] = row[7] = v;
        return;
        }
    a0 = (W4 * row[0]) + (1 << (ROW_SHIFT - 1));
    a1 = a0; a2 = a0; a3 = a0;
    a0 += W2 * row[2];
    a1 += W6 * row[2];
    a2 -= W6 * row[2];
    a3 -= W2 * row[2];

    b0 = W1 * row[1] + W3 * row[3];
    b1 = W3 * row[1] - W7 * row[3];
    b2 = W5 * row[1] - W1 * row[3];
    b3 = W7 * row[1] - W5 * row[3];

    if (row[4] | row[5] | row[6] | row[7])
        {
        a0 +=  W4 * row[4] + W6 * row[6];
        a1 += -W4 * row[4] - W2 * row[6];
        a2 += -W4 * row[4] + W2 * row[6];
        a3 +=  W4 * row[4] - W6 * row[6];

        b0 +=  W5 * row[5] + W7 * row[7];
        b1 += -W1 * row[5] - W5 * row[7];
        b2 +=  W7 * row[5] + W3 * row[7];
        b3 +=  W3 * row[5] - W1 * row[7];
        }
    /* the reference stores rows as int16 */
    row[0] = (s16)((a0 + b0) >> ROW_SHIFT);
    row[7] = (s16)((a0 - b0) >> ROW_SHIFT);
    row[1] = (s16)((a1 + b1) >> ROW_SHIFT);
    row[6] = (s16)((a1 - b1) >> ROW_SHIFT);
    row[2] = (s16)((a2 + b2) >> ROW_SHIFT);
    row[5] = (s16)((a2 - b2) >> ROW_SHIFT);
    row[3] = (s16)((a3 + b3) >> ROW_SHIFT);
    row[4] = (s16)((a3 - b3) >> ROW_SHIFT);
    }

/* column pass; results in out[0..7] (already shifted). hi = rows 4..7 may be non-zero */
static inline void idct_col(const int* col, int* out, int hi)
    {
    int a0, a1, a2, a3, b0, b1, b2, b3;
    a0 = W4 * (col[8 * 0] + ((1 << (COL_SHIFT - 1)) / W4));
    a1 = a0; a2 = a0; a3 = a0;
    a0 +=  W2 * col[8 * 2];
    a1 +=  W6 * col[8 * 2];
    a2 += -W6 * col[8 * 2];
    a3 += -W2 * col[8 * 2];

    b0 = W1 * col[8 * 1];
    b1 = W3 * col[8 * 1];
    b2 = W5 * col[8 * 1];
    b3 = W7 * col[8 * 1];

    b0 +=  W3 * col[8 * 3];
    b1 += -W7 * col[8 * 3];
    b2 += -W1 * col[8 * 3];
    b3 += -W5 * col[8 * 3];

    if (hi)
        {
        if (col[8 * 4])
            {
            a0 +=  W4 * col[8 * 4];
            a1 += -W4 * col[8 * 4];
            a2 += -W4 * col[8 * 4];
            a3 +=  W4 * col[8 * 4];
            }
        if (col[8 * 5])
            {
            b0 +=  W5 * col[8 * 5];
            b1 += -W1 * col[8 * 5];
            b2 +=  W7 * col[8 * 5];
            b3 +=  W3 * col[8 * 5];
            }
        if (col[8 * 6])
            {
            a0 +=  W6 * col[8 * 6];
            a1 += -W2 * col[8 * 6];
            a2 +=  W2 * col[8 * 6];
            a3 += -W6 * col[8 * 6];
            }
        if (col[8 * 7])
            {
            b0 +=  W7 * col[8 * 7];
            b1 += -W5 * col[8 * 7];
            b2 +=  W3 * col[8 * 7];
            b3 += -W1 * col[8 * 7];
            }
        }
    out[0] = (a0 + b0) >> COL_SHIFT;
    out[1] = (a1 + b1) >> COL_SHIFT;
    out[2] = (a2 + b2) >> COL_SHIFT;
    out[3] = (a3 + b3) >> COL_SHIFT;
    out[4] = (a3 - b3) >> COL_SHIFT;
    out[5] = (a2 - b2) >> COL_SHIFT;
    out[6] = (a1 - b1) >> COL_SHIFT;
    out[7] = (a0 - b0) >> COL_SHIFT;
    }

static void clear_rows(int* block, int rowmask)
    {
    int r;
    for (r = 0; r < 8; r++)
        if (rowmask & (1 << r))
            {
            int* p = block + 8 * r;
            p[0] = p[1] = p[2] = p[3] = p[4] = p[5] = p[6] = p[7] = 0;
            }
    }

static void idct_do(u8* dest, int stride, int* block, int rowmask, int add)
    {
    int i, k;
    int o[8];
    for (i = 0; i < 8; i++) if (rowmask & (1 << i)) idct_row(block + i * 8);
    if (rowmask == 1)
        {
        /* only the first row: every column is constant (exactly what idct_col gives) */
        for (i = 0; i < 8; i++) o[i] = (W4 * (block[i] + ((1 << (COL_SHIFT - 1)) / W4))) >> COL_SHIFT;
        for (k = 0; k < 8; k++)
            {
            u8* d = dest + k * stride;
            if (add) for (i = 0; i < 8; i++) d[i] = clip8(d[i] + o[i]);
            else for (i = 0; i < 8; i++) d[i] = clip8(o[i]);
            }
        }
    else
        {
        int hi = rowmask & 0xF0;
        for (i = 0; i < 8; i++)
            {
            u8* d = dest + i;
            idct_col(block + i, o, hi);
            if (add) for (k = 0; k < 8; k++) { d[0] = clip8(d[0] + o[k]); d += stride; }
            else for (k = 0; k < 8; k++) { d[0] = clip8(o[k]); d += stride; }
            }
        }
    clear_rows(block, rowmask);
    }

void idct_put(u8* dest, int stride, int* block)
    {
    idct_do(dest, stride, block, 0xFF, 0);
    }

void idct_add_rows(u8* dest, int stride, int* block, int rowmask)
    {
    if (!rowmask) return;
    idct_do(dest, stride, block, rowmask, 1);
    }

void idct_add(u8* dest, int stride, int* block)
    {
    idct_do(dest, stride, block, 0xFF, 1);
    }
