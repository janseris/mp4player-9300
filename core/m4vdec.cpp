/*
 * MPEG-4 Part 2 Simple Profile / H.263 baseline video decoder.
 * See m4vdec.h for what is supported. Written for MP4 Player 9300 (Nokia 9300, Symbian 7.0s),
 * portable C-style C++ (GCC 2.9 compatible, no writable static data).
 *
 * Bitstream semantics follow ISO/IEC 14496-2 and ITU-T H.263; behaviour in corner cases
 * (DC clipping, intra_dc_vlc_thr, chroma MV rounding) matches FFmpeg so that the output can be
 * compared bit-exactly with ffmpeg on a PC.
 */
#include "m4vdec.h"
#include "m4vtables.h"
#include "idct.h"

#define PAD   32        /* luma border (pixels) */
#define CPAD  16        /* chroma border */

/* ------------------------------------------------------------------ bit reader */

struct Bits
    {
    const u8* buf;
    int pos;        /* bit position */
    int size;       /* size in bits */
    };

static inline u32 bshow32(const Bits* b)
    {
    const u8* p = b->buf + (b->pos >> 3);
    u32 v = ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | (u32)p[3];
    int s = b->pos & 7;
    if (s) v = (v << s) | ((u32)p[4] >> (8 - s));
    return v;
    }
static inline u32 bshow(const Bits* b, int n) { return bshow32(b) >> (32 - n); }
static inline void bskip(Bits* b, int n) { b->pos += n; }
static inline u32 bget(Bits* b, int n) { u32 v = bshow32(b) >> (32 - n); b->pos += n; return v; }
static inline u32 bget1(Bits* b)
    {
    u32 v = (b->buf[b->pos >> 3] >> (7 - (b->pos & 7))) & 1;
    b->pos++;
    return v;
    }
static inline int bleft(const Bits* b) { return b->size - b->pos; }
static inline void balign(Bits* b) { b->pos = (b->pos + 7) & ~7; }

/* ------------------------------------------------------------------ VLC lookup tables */

/* small tables: entry = (symbol << 5) | length, 0 = invalid code */
#define LUT_LEN(e)  ((e) & 31)
#define LUT_SYM(e)  ((e) >> 5)
/* AC tables: len 0-4, last 5, run 6-11, level 12-19, escape 20 */
#define AC_LEN(e)   ((e) & 31)
#define AC_LAST(e)  (((e) >> 5) & 1)
#define AC_RUN(e)   (((e) >> 6) & 63)
#define AC_LEVEL(e) (((e) >> 12) & 255)
#define AC_ESC      (1 << 20)

#define AC_BITS   12
#define MV_BITS   12
#define DCL_BITS  11
#define DCC_BITS  12
#define CBPY_BITS 6
#define MCBPC_BITS 9

static void lut_put(u16* lut, int bits, int code, int len, int sym)
    {
    int shift, first, count, i;
    if (len <= 0 || len > bits) return;
    shift = bits - len;
    first = code << shift;
    count = 1 << shift;
    for (i = 0; i < count; i++) lut[first + i] = (u16)((sym << 5) | len);
    }

static void ac_lut_build(u32* lut, const u16* vlc, const s8* run, const s8* level, int lastStart)
    {
    int i, k;
    for (i = 0; i <= RL_N; i++)
        {
        int code = vlc[2 * i], len = vlc[2 * i + 1];
        int shift = AC_BITS - len;
        u32 e;
        if (i == RL_N) e = (u32)len | AC_ESC;
        else e = (u32)len | ((u32)(i >= lastStart) << 5) | ((u32)run[i] << 6) | ((u32)level[i] << 12);
        for (k = 0; k < (1 << shift); k++) lut[(code << shift) + k] = e;
        }
    }

static void rl_max_build(u8 maxLevel[2][64], u8 maxRun[2][64], const s8* run, const s8* level, int lastStart)
    {
    int i;
    for (i = 0; i < RL_N; i++)
        {
        int last = i >= lastStart;
        int r = run[i], l = level[i];
        if (l > maxLevel[last][r]) maxLevel[last][r] = (u8)l;
        if (r > maxRun[last][l]) maxRun[last][l] = (u8)r;
        }
    }

/* ------------------------------------------------------------------ decoder context */

struct Frame
    {
    u8* base;
    u8* y;
    u8* u;
    u8* v;
    };

struct M4vDec
    {
    MpAlloc al;

    /* lookup tables (built once) */
    u32 lutIntra[1 << AC_BITS];
    u32 lutInter[1 << AC_BITS];
    u16 lutMv[1 << MV_BITS];
    u16 lutDcL[1 << DCL_BITS];
    u16 lutDcC[1 << DCC_BITS];
    u16 lutCbpy[1 << CBPY_BITS];
    u16 lutMcbpcI[1 << MCBPC_BITS];
    u16 lutMcbpcP[1 << MCBPC_BITS];
    u8 maxLevelIntra[2][64], maxRunIntra[2][64];
    u8 maxLevelInter[2][64], maxRunInter[2][64];

    /* sequence */
    int haveVol;
    int shortHeader;        /* stream is H.263 / short header */
    int width, height;      /* display size */
    int mbw, mbh, mbnum;
    int ystride, cstride;
    int verid;
    int quantType;          /* 0 = H.263, 1 = MPEG */
    u8 intraMat[64], interMat[64];  /* raster order */
    int timeIncBits;
    int resyncMarker;
    int quantPrecision;
    int cplxTrashI, cplxTrashP;
    int unsupported;        /* reason code when the VOL uses features we can't decode */

    /* pictures */
    Frame frm[2];
    int cur;                /* index of the frame being decoded */
    int haveRef;            /* frm[cur ^ 1] holds a valid reference */

    /* per-MB / per-block state of the current VOP */
    u8* mbIntra;
    u8* mbQs;
    u16* mbSlice;           /* 0xFFFF = not decoded yet */
    s16* mv;                /* 2 per 8x8 block, (2*mbw) x (2*mbh) */
    s16* dcY; s16* dcU; s16* dcV;
    s16* acY; s16* acU; s16* acV;

    /* VOP state */
    int vopType;            /* 0 I, 1 P, 2 B, 3 S */
    int rounding;
    int qscale;
    int fcode;
    int intraDcThr;
    int slice;

    int blk[64];            /* coefficients; kept all zero between blocks (the IDCT clears what it used) */
    u8 nzPos[64];           /* raster positions of the coefficients read into blk */
    int nzCount;
    int nzRows;             /* bit r: row r of blk has a coefficient */
    char err[48];
    char info[48];
    };

/* ------------------------------------------------------------------ small helpers */

static void set_err(M4vDec* d, const char* s)
    {
    int i = 0;
    while (s[i] && i < (int)sizeof(d->err) - 1) { d->err[i] = s[i]; i++; }
    d->err[i] = 0;
    }

static void append(char* dst, int cap, int* n, const char* s)
    {
    while (*s && *n < cap - 1) dst[(*n)++] = *s++;
    dst[*n] = 0;
    }

static void append_int(char* dst, int cap, int* n, int v)
    {
    char t[12];
    int k = 0;
    if (v < 0) { append(dst, cap, n, "-"); v = -v; }
    do { t[k++] = (char)('0' + v % 10); v /= 10; } while (v && k < 11);
    while (k > 0 && *n < cap - 1) dst[(*n)++] = t[--k];
    dst[*n] = 0;
    }

static void set_info(M4vDec* d)
    {
    int n = 0;
    append(d->info, sizeof(d->info), &n, d->shortHeader ? "H.263 " : "MPEG-4 Part 2 ");
    append_int(d->info, sizeof(d->info), &n, d->width);
    append(d->info, sizeof(d->info), &n, "x");
    append_int(d->info, sizeof(d->info), &n, d->height);
    if (!d->shortHeader && d->quantType) append(d->info, sizeof(d->info), &n, " MPEG-quant");
    }

static int ilog2(u32 v) { int n = 0; while (v > 1) { v >>= 1; n++; } return n; }

static inline int mid_pred(int a, int b, int c)
    {
    if (a > b) { if (c > b) { if (c > a) b = a; else b = c; } }
    else { if (b > c) { if (c > a) b = c; else b = a; } }
    return b;
    }

/* ------------------------------------------------------------------ open / close / buffers */

static void free_buffers(M4vDec* d)
    {
    int i;
    for (i = 0; i < 2; i++) { mp_free(&d->al, d->frm[i].base); d->frm[i].base = 0; }
    mp_free(&d->al, d->mbIntra); d->mbIntra = 0;
    mp_free(&d->al, d->mbQs); d->mbQs = 0;
    mp_free(&d->al, d->mbSlice); d->mbSlice = 0;
    mp_free(&d->al, d->mv); d->mv = 0;
    mp_free(&d->al, d->dcY); d->dcY = 0;
    mp_free(&d->al, d->dcU); d->dcU = 0;
    mp_free(&d->al, d->dcV); d->dcV = 0;
    mp_free(&d->al, d->acY); d->acY = 0;
    mp_free(&d->al, d->acU); d->acU = 0;
    mp_free(&d->al, d->acV); d->acV = 0;
    d->mbw = d->mbh = d->mbnum = 0;
    d->haveRef = 0;
    }

/* (re)allocates everything for a picture size; 0 or error */
static int set_size(M4vDec* d, int w, int h)
    {
    int mbw, mbh, i, ysize, csize, nb;
    if (w <= 0 || h <= 0 || w > 2048 || h > 2048) { set_err(d, "bad picture size"); return M4V_ERR_UNSUPPORTED; }
    mbw = (w + 15) >> 4;
    mbh = (h + 15) >> 4;
    d->width = w;
    d->height = h;
    if (mbw == d->mbw && mbh == d->mbh && d->frm[0].base) return 0;
    free_buffers(d);
    d->mbw = mbw; d->mbh = mbh; d->mbnum = mbw * mbh;
    d->ystride = mbw * 16 + 2 * PAD;
    d->cstride = mbw * 8 + 2 * CPAD;
    ysize = d->ystride * (mbh * 16 + 2 * PAD);
    csize = d->cstride * (mbh * 8 + 2 * CPAD);
    for (i = 0; i < 2; i++)
        {
        Frame* f = &d->frm[i];
        f->base = (u8*)mp_alloc(&d->al, ysize + 2 * csize);
        if (!f->base) { free_buffers(d); set_err(d, "out of memory"); return M4V_ERR_NOMEM; }
        f->y = f->base + PAD * d->ystride + PAD;
        f->u = f->base + ysize + CPAD * d->cstride + CPAD;
        f->v = f->base + ysize + csize + CPAD * d->cstride + CPAD;
        mp_memset(f->base, 16, ysize);
        mp_memset(f->base + ysize, 128, 2 * csize);
        }
    nb = 4 * d->mbnum;
    d->mbIntra = (u8*)mp_alloc(&d->al, d->mbnum);
    d->mbQs = (u8*)mp_alloc(&d->al, d->mbnum);
    d->mbSlice = (u16*)mp_alloc(&d->al, d->mbnum * 2);
    d->mv = (s16*)mp_alloc(&d->al, nb * 2 * 2);
    d->dcY = (s16*)mp_alloc(&d->al, nb * 2);
    d->dcU = (s16*)mp_alloc(&d->al, d->mbnum * 2);
    d->dcV = (s16*)mp_alloc(&d->al, d->mbnum * 2);
    d->acY = (s16*)mp_alloc(&d->al, nb * 16 * 2);
    d->acU = (s16*)mp_alloc(&d->al, d->mbnum * 16 * 2);
    d->acV = (s16*)mp_alloc(&d->al, d->mbnum * 16 * 2);
    if (!d->mbIntra || !d->mbQs || !d->mbSlice || !d->mv || !d->dcY || !d->dcU || !d->dcV ||
        !d->acY || !d->acU || !d->acV)
        {
        free_buffers(d);
        set_err(d, "out of memory");
        return M4V_ERR_NOMEM;
        }
    d->cur = 0;
    d->haveRef = 0;
    set_info(d);
    return 0;
    }

int m4v_open(M4vDec** aDec, const MpAlloc* aAlloc)
    {
    M4vDec* d;
    int i;
    *aDec = 0;
    d = (M4vDec*)mp_alloc(aAlloc, sizeof(M4vDec));
    if (!d) return M4V_ERR_NOMEM;
    d->al = *aAlloc;

    ac_lut_build(d->lutIntra, kIntraVlc, kIntraRun, kIntraLevel, INTRA_RL_LAST);
    ac_lut_build(d->lutInter, kInterVlc, kInterRun, kInterLevel, INTER_RL_LAST);
    rl_max_build(d->maxLevelIntra, d->maxRunIntra, kIntraRun, kIntraLevel, INTRA_RL_LAST);
    rl_max_build(d->maxLevelInter, d->maxRunInter, kInterRun, kInterLevel, INTER_RL_LAST);
    for (i = 0; i < 33; i++) lut_put(d->lutMv, MV_BITS, kMvTab[2 * i], kMvTab[2 * i + 1], i);
    for (i = 0; i < 13; i++)
        {
        lut_put(d->lutDcL, DCL_BITS, kDcLum[2 * i], kDcLum[2 * i + 1], i);
        lut_put(d->lutDcC, DCC_BITS, kDcChrom[2 * i], kDcChrom[2 * i + 1], i);
        }
    for (i = 0; i < 16; i++) lut_put(d->lutCbpy, CBPY_BITS, kCbpyTab[2 * i], kCbpyTab[2 * i + 1], i);
    for (i = 0; i < 9; i++) lut_put(d->lutMcbpcI, MCBPC_BITS, kIntraMcbpcCode[i], kIntraMcbpcBits[i], i);
    for (i = 0; i < 28; i++) lut_put(d->lutMcbpcP, MCBPC_BITS, kInterMcbpcCode[i], kInterMcbpcBits[i], i);

    for (i = 0; i < 64; i++) { d->intraMat[i] = kDefIntraMat[i]; d->interMat[i] = kDefInterMat[i]; }
    d->quantPrecision = 5;
    d->timeIncBits = 1;
    set_err(d, "");
    d->info[0] = 0;
    *aDec = d;
    return 0;
    }

void m4v_close(M4vDec* d)
    {
    MpAlloc al;
    if (!d) return;
    al = d->al;
    free_buffers(d);
    mp_free(&al, d);
    }

void m4v_flush(M4vDec* d) { d->haveRef = 0; }

int m4v_size(M4vDec* d, int* w, int* h)
    {
    if (!d->mbw) return 0;
    *w = d->width;
    *h = d->height;
    return 1;
    }

const char* m4v_error_text(M4vDec* d) { return d->err; }
const char* m4v_stream_info(M4vDec* d) { return d->info; }

void m4v_params(M4vDec* d, M4vParams* p)
    {
    p->haveVol = d->haveVol;
    p->shortHeader = d->shortHeader;
    p->verid = d->verid;
    p->quantType = d->quantType;
    p->resyncMarker = d->resyncMarker;
    p->timeIncBits = d->timeIncBits;
    p->width = d->width;
    p->height = d->height;
    }

/* ------------------------------------------------------------------ headers */

static int parse_vol(M4vDec* d, Bits* b)
    {
    int shape, w, h, isIntraMat, i, v, last, resolution, sprite, notBit8;
    d->unsupported = 0;
    bskip(b, 1);                                /* random_accessible_vol */
    bskip(b, 8);                                /* video_object_type_indication */
    if (bget1(b)) { d->verid = bget(b, 4); bskip(b, 3); }
    else d->verid = 1;
    if (bget(b, 4) == 15) bskip(b, 16);         /* aspect ratio: extended PAR */
    if (bget1(b))                               /* vol_control_parameters */
        {
        bskip(b, 2);                            /* chroma_format */
        bskip(b, 1);                            /* low_delay */
        if (bget1(b)) bskip(b, 15 + 1 + 15 + 1 + 15 + 1 + 3 + 11 + 1 + 15 + 1);
        }
    shape = bget(b, 2);
    if (shape != 0) { set_err(d, "non-rectangular VOL"); d->unsupported = 1; return M4V_ERR_UNSUPPORTED; }
    bskip(b, 1);                                /* marker */
    resolution = bget(b, 16);
    if (resolution == 0) { set_err(d, "bad VOL (time resolution)"); return M4V_ERR_BITSTREAM; }
    d->timeIncBits = ilog2(resolution - 1) + 1;
    if (d->timeIncBits < 1) d->timeIncBits = 1;
    bskip(b, 1);                                /* marker */
    if (bget1(b)) bskip(b, d->timeIncBits);     /* fixed_vop_rate */
    bskip(b, 1);
    w = bget(b, 13);
    bskip(b, 1);
    h = bget(b, 13);
    bskip(b, 1);
    if (bget1(b)) { set_err(d, "interlaced video"); d->unsupported = 1; return M4V_ERR_UNSUPPORTED; }
    bskip(b, 1);                                /* obmc_disable */
    sprite = (d->verid == 1) ? (int)bget1(b) : (int)bget(b, 2);
    if (sprite) { set_err(d, "GMC/sprites (Advanced Simple)"); d->unsupported = 1; return M4V_ERR_UNSUPPORTED; }
    notBit8 = bget1(b);
    if (notBit8)
        {
        d->quantPrecision = bget(b, 4);
        if (bget(b, 4) != 8) { set_err(d, "not 8-bit video"); d->unsupported = 1; return M4V_ERR_UNSUPPORTED; }
        if (d->quantPrecision < 3 || d->quantPrecision > 9) d->quantPrecision = 5;
        }
    else d->quantPrecision = 5;
    d->quantType = bget1(b);
    for (i = 0; i < 64; i++) { d->intraMat[i] = kDefIntraMat[i]; d->interMat[i] = kDefInterMat[i]; }
    if (d->quantType)
        {
        for (isIntraMat = 1; isIntraMat >= 0; isIntraMat--)
            {
            u8* mat = isIntraMat ? d->intraMat : d->interMat;
            if (bget1(b))
                {
                last = 0;
                for (i = 0; i < 64; i++)
                    {
                    v = bget(b, 8);
                    if (v == 0) break;
                    last = v;
                    mat[kScanZigzag[i]] = (u8)v;
                    }
                for (; i < 64; i++) mat[kScanZigzag[i]] = (u8)last;
                }
            }
        }
    if (d->verid != 1 && bget1(b)) { set_err(d, "quarter-pel (Advanced Simple)"); d->unsupported = 1; return M4V_ERR_UNSUPPORTED; }
    d->cplxTrashI = d->cplxTrashP = 0;
    if (!bget1(b))                              /* complexity_estimation_disable */
        {
        int method = bget(b, 2);
        if (method < 2)
            {
            if (!bget1(b)) { d->cplxTrashI += 8 * (bget1(b) + bget1(b) + bget1(b) + bget1(b) + bget1(b) + bget1(b)); }
            if (!bget1(b))
                {
                d->cplxTrashI += 8 * bget1(b);
                d->cplxTrashP += 8 * (bget1(b) + bget1(b));
                d->cplxTrashI += 8 * bget1(b);
                }
            bskip(b, 1);
            if (!bget1(b)) { d->cplxTrashI += 8 * (bget1(b) + bget1(b) + bget1(b)) + 4 * bget1(b); }
            if (!bget1(b))
                {
                d->cplxTrashP += 8 * (bget1(b) + bget1(b));
                bskip(b, 1);                    /* interpolate_mc_q (B only) */
                d->cplxTrashP += 8 * (bget1(b) + bget1(b) + bget1(b));
                }
            bskip(b, 1);
            if (method == 1) { d->cplxTrashI += 8 * bget1(b); d->cplxTrashP += 8 * bget1(b); }
            }
        }
    d->resyncMarker = !bget1(b);
    if (bget1(b)) { set_err(d, "data partitioning"); d->unsupported = 1; return M4V_ERR_UNSUPPORTED; }
    if (d->verid != 1)
        {
        if (bget1(b)) { set_err(d, "NEWPRED"); d->unsupported = 1; return M4V_ERR_UNSUPPORTED; }
        if (bget1(b)) { set_err(d, "reduced resolution VOP"); d->unsupported = 1; return M4V_ERR_UNSUPPORTED; }
        }
    /* scalability bit ignored (base layer decodes anyway) */
    d->shortHeader = 0;
    v = set_size(d, w, h);
    if (v < 0) return v;
    d->haveVol = 1;
    set_info(d);
    return 0;
    }

/* ------------------------------------------------------------------ prediction helpers */

/* 1 if MB (mx,my) is inside the picture and belongs to the current video packet */
static inline int mb_avail(const M4vDec* d, int mx, int my)
    {
    if (mx < 0 || my < 0 || mx >= d->mbw || my >= d->mbh) return 0;
    return d->mbSlice[my * d->mbw + mx] == d->slice;
    }

/* motion vector predictor for 8x8 block 'blk' (0..3) of MB (mx,my) */
static void mv_pred(const M4vDec* d, int mx, int my, int blk, int* px, int* py)
    {
    int bw = 2 * d->mbw;
    int bx = 2 * mx + (blk & 1), by = 2 * my + (blk >> 1);
    int cx[3], cy[3], ok[3], n, i, valid;
    /* candidates: A left, B above, C above-right (relative positions as in 14496-2 7.6.5) */
    cx[0] = bx - 1; cy[0] = by;
    switch (blk)
        {
        case 0: cx[1] = bx; cy[1] = by - 1; cx[2] = bx + 2; cy[2] = by - 1; break;
        case 1: cx[1] = bx; cy[1] = by - 1; cx[2] = bx + 1; cy[2] = by - 1; break;
        case 2: cx[1] = bx; cy[1] = by - 1; cx[2] = bx + 1; cy[2] = by - 1; break;
        default: cx[1] = bx - 1; cy[1] = by - 1; cx[2] = bx; cy[2] = by - 1; break;
        }
    n = 0;
    for (i = 0; i < 3; i++)
        {
        int nmx = cx[i] >> 1, nmy = cy[i] >> 1;
        if (cx[i] < 0 || cy[i] < 0 || cx[i] >= bw) ok[i] = 0;
        else if (nmx == mx && nmy == my) ok[i] = 1;
        else ok[i] = mb_avail(d, nmx, nmy);
        n += ok[i];
        }
    if (n == 0) { *px = 0; *py = 0; return; }
    if (n == 1)
        {
        for (i = 0; i < 3; i++)
            if (ok[i])
                {
                const s16* m = d->mv + 2 * (cy[i] * bw + cx[i]);
                *px = m[0]; *py = m[1];
                return;
                }
        }
    {
    int ax[3], ay[3];
    for (i = 0; i < 3; i++)
        {
        valid = ok[i];
        if (valid) { const s16* m = d->mv + 2 * (cy[i] * bw + cx[i]); ax[i] = m[0]; ay[i] = m[1]; }
        else { ax[i] = 0; ay[i] = 0; }
        }
    *px = mid_pred(ax[0], ax[1], ax[2]);
    *py = mid_pred(ay[0], ay[1], ay[2]);
    }
    }

static int decode_mv_comp(M4vDec* d, Bits* b, int pred, int fcode)
    {
    int e = d->lutMv[bshow(b, MV_BITS)];
    int code, val, shift, range;
    if (!LUT_LEN(e)) return 0x10000;
    bskip(b, LUT_LEN(e));
    code = LUT_SYM(e);
    if (code == 0) return pred;
    shift = fcode - 1;
    {
    int sign = bget1(b);
    val = code;
    if (shift) { val = ((val - 1) << shift) | (int)bget(b, shift); val++; }
    if (sign) val = -val;
    }
    val += pred;
    range = 32 << shift;
    val = ((val + range) & (2 * range - 1)) - range;
    return val;
    }

/* ------------------------------------------------------------------ motion compensation */

static void mc_block(u8* dst, int ds, const u8* src, int ss, int dxy, int w, int h, int rnd)
    {
    int x, y;
    switch (dxy)
        {
        case 0:
            for (y = 0; y < h; y++)
                {
                mp_memcpy(dst, src, w);
                dst += ds; src += ss;
                }
            break;
        case 1:
            for (y = 0; y < h; y++)
                {
                for (x = 0; x < w; x++) dst[x] = (u8)((src[x] + src[x + 1] + 1 - rnd) >> 1);
                dst += ds; src += ss;
                }
            break;
        case 2:
            for (y = 0; y < h; y++)
                {
                for (x = 0; x < w; x++) dst[x] = (u8)((src[x] + src[x + ss] + 1 - rnd) >> 1);
                dst += ds; src += ss;
                }
            break;
        default:
            for (y = 0; y < h; y++)
                {
                for (x = 0; x < w; x++)
                    dst[x] = (u8)((src[x] + src[x + 1] + src[x + ss] + src[x + ss + 1] + 2 - rnd) >> 2);
                dst += ds; src += ss;
                }
            break;
        }
    }

/* predicts a w x h block at plane position (px,py) moved by half-pel vector (vx,vy) */
static void mc_plane(u8* dstPlane, const u8* refPlane, int stride, int px, int py, int vx, int vy,
                     int w, int h, int planeW, int planeH, int rnd)
    {
    int sx = px + (vx >> 1), sy = py + (vy >> 1);
    int dxy = (vx & 1) | ((vy & 1) << 1);
    if (sx < -(w + 1)) sx = -(w + 1);
    if (sx > planeW) sx = planeW;
    if (sy < -(h + 1)) sy = -(h + 1);
    if (sy > planeH) sy = planeH;
    mc_block(dstPlane + py * stride + px, stride, refPlane + sy * stride + sx, stride, dxy, w, h, rnd);
    }

static void copy_mb(M4vDec* d, const Frame* ref, Frame* cur, int mx, int my)
    {
    int pw = d->mbw * 16, ph = d->mbh * 16;
    mc_plane(cur->y, ref->y, d->ystride, mx * 16, my * 16, 0, 0, 16, 16, pw, ph, 0);
    mc_plane(cur->u, ref->u, d->cstride, mx * 8, my * 8, 0, 0, 8, 8, pw / 2, ph / 2, 0);
    mc_plane(cur->v, ref->v, d->cstride, mx * 8, my * 8, 0, 0, 8, 8, pw / 2, ph / 2, 0);
    }

/* 14496-2 table 7-9 (sixteenth sample rounding), as FFmpeg's ff_h263_round_chroma */
static const u8 kChromaRound[16] = { 0, 0, 0, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 1, 1 };
static inline int round_chroma4(int x) { return kChromaRound[x & 15] + (x >> 3); }

static void motion_comp(M4vDec* d, int mx, int my, int four)
    {
    Frame* cur = &d->frm[d->cur];
    const Frame* ref = &d->frm[d->cur ^ 1];
    int pw = d->mbw * 16, ph = d->mbh * 16;
    int bw = 2 * d->mbw;
    const s16* m = d->mv + 2 * ((2 * my) * bw + 2 * mx);
    int cmx, cmy;
    if (!four)
        {
        mc_plane(cur->y, ref->y, d->ystride, mx * 16, my * 16, m[0], m[1], 16, 16, pw, ph, d->rounding);
        cmx = (m[0] >> 1) | (m[0] & 1);
        cmy = (m[1] >> 1) | (m[1] & 1);
        }
    else
        {
        int i, sx = 0, sy = 0;
        for (i = 0; i < 4; i++)
            {
            const s16* v = d->mv + 2 * ((2 * my + (i >> 1)) * bw + 2 * mx + (i & 1));
            mc_plane(cur->y, ref->y, d->ystride, mx * 16 + (i & 1) * 8, my * 16 + (i >> 1) * 8,
                     v[0], v[1], 8, 8, pw, ph, d->rounding);
            sx += v[0]; sy += v[1];
            }
        cmx = round_chroma4(sx);
        cmy = round_chroma4(sy);
        }
    mc_plane(cur->u, ref->u, d->cstride, mx * 8, my * 8, cmx, cmy, 8, 8, pw / 2, ph / 2, d->rounding);
    mc_plane(cur->v, ref->v, d->cstride, mx * 8, my * 8, cmx, cmy, 8, 8, pw / 2, ph / 2, d->rounding);
    }

/* ------------------------------------------------------------------ texture */

/* reads one run/level event with MPEG-4 escapes; returns 0 or -1 */
static inline int read_event(M4vDec* d, Bits* b, const u32* lut, u8 (*maxLevel)[64],
                             u8 (*maxRun)[64], int shortHdr, int* last, int* run, int* level)
    {
    u32 e = lut[bshow(b, AC_BITS)];
    int len = AC_LEN(e);
    if (!len) return -1;
    bskip(b, len);
    if (!(e & AC_ESC))
        {
        *last = AC_LAST(e); *run = AC_RUN(e);
        *level = bget1(b) ? -(int)AC_LEVEL(e) : (int)AC_LEVEL(e);
        return 0;
        }
    if (shortHdr)
        {
        int l;
        *last = bget1(b);
        *run = bget(b, 6);
        l = bget(b, 8);
        if (l == 0 || l == 128) return -1;
        *level = (l > 128) ? l - 256 : l;
        return 0;
        }
    if (!bget1(b))
        {
        /* type 1: level offset */
        int l;
        e = lut[bshow(b, AC_BITS)];
        len = AC_LEN(e);
        if (!len || (e & AC_ESC)) return -1;
        bskip(b, len);
        *last = AC_LAST(e); *run = AC_RUN(e);
        l = AC_LEVEL(e) + maxLevel[*last][*run];
        *level = bget1(b) ? -l : l;
        return 0;
        }
    if (!bget1(b))
        {
        /* type 2: run offset */
        int l;
        e = lut[bshow(b, AC_BITS)];
        len = AC_LEN(e);
        if (!len || (e & AC_ESC)) return -1;
        bskip(b, len);
        *last = AC_LAST(e);
        l = AC_LEVEL(e);
        *run = AC_RUN(e) + maxRun[*last][l] + 1;
        *level = bget1(b) ? -l : l;
        return 0;
        }
    /* type 3: fixed length */
    {
    int l;
    *last = bget1(b);
    *run = bget(b, 6);
    bskip(b, 1);
    l = bget(b, 12);
    bskip(b, 1);
    if (l & 0x800) l -= 4096;
    if (l == 0) return -1;
    *level = l;
    }
    return 0;
    }

/* decodes AC events into blk[scan[pos]] starting at scan position pos (quantised levels) */
static int read_coeffs(M4vDec* d, Bits* b, int* blk, int pos, const u8* scan, int intraTab, int shortHdr)
    {
    const u32* lut = intraTab ? d->lutIntra : d->lutInter;
    u8 (*maxLevel)[64] = intraTab ? d->maxLevelIntra : d->maxLevelInter;
    u8 (*maxRun)[64] = intraTab ? d->maxRunIntra : d->maxRunInter;
    int last, run, level;
    d->nzCount = 0;
    d->nzRows = 0;
    for (;;)
        {
        if (read_event(d, b, lut, maxLevel, maxRun, shortHdr, &last, &run, &level) < 0) return -1;
        pos += run;
        if (pos > 63) return -1;
        {
        int rp = scan[pos];
        blk[rp] = level;
        d->nzPos[d->nzCount++] = (u8)rp;
        d->nzRows |= 1 << (rp >> 3);
        }
        pos++;
        if (last) return 0;
        }
    }

static inline int sat(int v) { return v < -2048 ? -2048 : (v > 2047 ? 2047 : v); }

/* dequantises an inter block in place (levels -> coefficients) */
static void dequant_inter(M4vDec* d, int* blk)
    {
    int q = d->qscale, i;
    if (!d->quantType || d->shortHeader)
        {
        int qmul = q << 1, qadd = (q - 1) | 1;
        for (i = 0; i < d->nzCount; i++)
            {
            int p = d->nzPos[i];
            int l = blk[p];
            blk[p] = sat(l > 0 ? l * qmul + qadd : l * qmul - qadd);
            }
        }
    else
        {
        int sum = 0;
        for (i = 0; i < d->nzCount; i++)
            {
            int p = d->nzPos[i];
            int l = blk[p];
            int a = l < 0 ? -l : l;
            a = (((a << 1) + 1) * q * d->interMat[p]) >> 4;
            l = sat(l < 0 ? -a : a);
            blk[p] = l;
            sum += l;
            }
        if (!(sum & 1))
            {
            blk[63] ^= 1;
            d->nzRows |= 0x80;
            }
        }
    }

/* dequantises the AC part of an intra block (blk[0] already holds the DC coefficient) */
static void dequant_intra_ac(M4vDec* d, int* blk)
    {
    int q = d->qscale, i;
    if (!d->quantType || d->shortHeader)
        {
        int qmul = q << 1, qadd = (q - 1) | 1;
        for (i = 1; i < 64; i++)
            {
            int l = blk[i];
            if (l) blk[i] = (s16)sat(l > 0 ? l * qmul + qadd : l * qmul - qadd);
            }
        }
    else
        {
        int sum = blk[0];
        for (i = 1; i < 64; i++)
            {
            int l = blk[i];
            if (l)
                {
                int a = l < 0 ? -l : l;
                a = (a * q * d->intraMat[i]) >> 3;
                l = sat(l < 0 ? -a : a);
                blk[i] = l;
                sum += l;
                }
            }
        if (!(sum & 1)) blk[63] ^= 1;
        }
    }

/* MPEG-4 intra block: DC/AC prediction, coefficients, dequantisation. n = 0..5 */
static int decode_intra_block(M4vDec* d, Bits* b, int n, int coded, int useDcVlc, int acPred,
                              int mx, int my)
    {
    int* blk = d->blk;
    int bw = 2 * d->mbw;
    int fa, fb, fc, dir, pred, scale, level, i;
    s16* dcCur;
    s16* acCur;
    const s16* acA = 0;
    const s16* acC = 0;
    int qsA = d->qscale, qsC = d->qscale;
    const u8* scan;

    if (n < 4)
        {
        int bx = 2 * mx + (n & 1), by = 2 * my + (n >> 1);
        int aIn, bIn, cIn;      /* neighbour inside the current MB? */
        int aOk, bOk, cOk;
        aIn = (n & 1); bIn = (n == 3); cIn = (n >> 1);
        aOk = aIn ? 1 : (bx > 0 && mb_avail(d, (bx - 1) >> 1, my) && d->mbIntra[my * d->mbw + ((bx - 1) >> 1)]);
        cOk = cIn ? 1 : (by > 0 && mb_avail(d, mx, (by - 1) >> 1) && d->mbIntra[((by - 1) >> 1) * d->mbw + mx]);
        if (bIn) bOk = 1;
        else if (bx == 0 || by == 0) bOk = 0;
        else
            {
            int nmx = (bx - 1) >> 1, nmy = (by - 1) >> 1;
            bOk = mb_avail(d, nmx, nmy) && d->mbIntra[nmy * d->mbw + nmx];
            }
        fa = aOk ? d->dcY[by * bw + bx - 1] : 1024;
        fb = bOk ? d->dcY[(by - 1) * bw + bx - 1] : 1024;
        fc = cOk ? d->dcY[(by - 1) * bw + bx] : 1024;
        dcCur = &d->dcY[by * bw + bx];
        acCur = &d->acY[(by * bw + bx) * 16];
        if (aOk) { acA = &d->acY[(by * bw + bx - 1) * 16]; if (!aIn) qsA = d->mbQs[my * d->mbw + mx - 1]; }
        if (cOk) { acC = &d->acY[((by - 1) * bw + bx) * 16]; if (!cIn) qsC = d->mbQs[(my - 1) * d->mbw + mx]; }
        scale = kYDcScale[d->qscale];
        }
    else
        {
        s16* dcp = (n == 4) ? d->dcU : d->dcV;
        s16* acp = (n == 4) ? d->acU : d->acV;
        int w = d->mbw, me = my * w + mx;
        int aOk = mx > 0 && mb_avail(d, mx - 1, my) && d->mbIntra[me - 1];
        int cOk = my > 0 && mb_avail(d, mx, my - 1) && d->mbIntra[me - w];
        int bOk = mx > 0 && my > 0 && mb_avail(d, mx - 1, my - 1) && d->mbIntra[me - w - 1];
        fa = aOk ? dcp[me - 1] : 1024;
        fb = bOk ? dcp[me - w - 1] : 1024;
        fc = cOk ? dcp[me - w] : 1024;
        dcCur = &dcp[me];
        acCur = &acp[me * 16];
        if (aOk) { acA = &acp[(me - 1) * 16]; qsA = d->mbQs[me - 1]; }
        if (cOk) { acC = &acp[(me - w) * 16]; qsC = d->mbQs[me - w]; }
        scale = kCDcScale[d->qscale];
        }
    if (mp_abs(fa - fb) < mp_abs(fb - fc)) { pred = fc; dir = 1; }  /* from above */
    else { pred = fa; dir = 0; }                                   /* from the left */

    if (acPred) scan = dir ? kScanAltH : kScanAltV;
    else scan = kScanZigzag;

    if (useDcVlc)
        {
        int size;
        u16 e = (n < 4) ? d->lutDcL[bshow(b, DCL_BITS)] : d->lutDcC[bshow(b, DCC_BITS)];
        if (!LUT_LEN(e)) return -1;
        bskip(b, LUT_LEN(e));
        size = LUT_SYM(e);
        level = 0;
        if (size)
            {
            int v = bget(b, size);
            if (!(v >> (size - 1))) v = v - (1 << size) + 1;
            level = v;
            if (size > 8) bskip(b, 1);
            }
        if (coded && read_coeffs(d, b, blk, 1, scan, 1, 0) < 0) return -1;
        }
    else
        {
        if (coded && read_coeffs(d, b, blk, 0, scan, 1, 0) < 0) return -1;
        level = blk[0];
        }

    /* DC */
    level += (pred + (scale >> 1)) / scale;
    {
    int dc = level * scale;
    if (dc & ~2047) dc = dc < 0 ? 0 : 2047;
    *dcCur = (s16)dc;
    }

    /* AC prediction */
    if (acPred)
        {
        if (dir == 0)
            {
            if (acA)
                {
                if (qsA == d->qscale) for (i = 1; i < 8; i++) blk[i << 3] = blk[i << 3] + acA[i];
                else
                    for (i = 1; i < 8; i++)
                        {
                        int t = acA[i] * qsA;
                        t = t >= 0 ? (t + (d->qscale >> 1)) / d->qscale : (t - (d->qscale >> 1)) / d->qscale;
                        blk[i << 3] = blk[i << 3] + t;
                        }
                }
            }
        else
            {
            if (acC)
                {
                if (qsC == d->qscale) for (i = 1; i < 8; i++) blk[i] = blk[i] + acC[8 + i];
                else
                    for (i = 1; i < 8; i++)
                        {
                        int t = acC[8 + i] * qsC;
                        t = t >= 0 ? (t + (d->qscale >> 1)) / d->qscale : (t - (d->qscale >> 1)) / d->qscale;
                        blk[i] = blk[i] + t;
                        }
                }
            }
        }
    for (i = 1; i < 8; i++) { acCur[i] = (s16)blk[i << 3]; acCur[8 + i] = (s16)blk[i]; }

    blk[0] = level * scale;
    dequant_intra_ac(d, blk);
    return 0;
    }

static u8* block_dst(M4vDec* d, int n, int mx, int my, int* stride)
    {
    Frame* f = &d->frm[d->cur];
    if (n < 4)
        {
        *stride = d->ystride;
        return f->y + (my * 16 + (n >> 1) * 8) * d->ystride + mx * 16 + (n & 1) * 8;
        }
    *stride = d->cstride;
    return ((n == 4) ? f->u : f->v) + my * 8 * d->cstride + mx * 8;
    }

/* ------------------------------------------------------------------ macroblocks */

static void mb_set_zero_mv(M4vDec* d, int mx, int my)
    {
    int bw = 2 * d->mbw;
    s16* m = d->mv + 2 * ((2 * my) * bw + 2 * mx);
    m[0] = m[1] = m[2] = m[3] = 0;
    m += 2 * bw;
    m[0] = m[1] = m[2] = m[3] = 0;
    }

static const s8 kDqTab[4] = { -1, -2, 1, 2 };

static int decode_mb(M4vDec* d, Bits* b, int mx, int my)
    {
    int mbi = my * d->mbw + mx;
    int mcbpc, intra, dquant, four = 0, cbpy, cbp, i, stride;
    int shortHdr = d->shortHeader;
    Frame* cur = &d->frm[d->cur];

    if (d->vopType == 1)
        {
        for (;;)
            {
            u16 e;
            if (bget1(b))
                {
                /* not coded: copy from the reference */
                d->mbSlice[mbi] = (u16)d->slice;
                d->mbIntra[mbi] = 0;
                d->mbQs[mbi] = (u8)d->qscale;
                mb_set_zero_mv(d, mx, my);
                copy_mb(d, &d->frm[d->cur ^ 1], cur, mx, my);
                return 0;
                }
            e = d->lutMcbpcP[bshow(b, MCBPC_BITS)];
            if (!LUT_LEN(e)) return -1;
            bskip(b, LUT_LEN(e));
            mcbpc = LUT_SYM(e);
            if (mcbpc != 20) break;
            }
        if (mcbpc > 20) return -1;                  /* INTER4V+Q: H.263+ only */
        intra = (mcbpc & 4) != 0;
        dquant = (mcbpc & 8) != 0;
        four = (mcbpc & 16) != 0;
        if (four && shortHdr) return -1;
        }
    else
        {
        for (;;)
            {
            u16 e = d->lutMcbpcI[bshow(b, MCBPC_BITS)];
            if (!LUT_LEN(e)) return -1;
            bskip(b, LUT_LEN(e));
            mcbpc = LUT_SYM(e);
            if (mcbpc != 8) break;
            }
        intra = 1;
        dquant = (mcbpc & 4) != 0;
        }

    d->mbSlice[mbi] = (u16)d->slice;
    if (intra)
        {
        int acPred = 0, useDcVlc;
        u16 e;
        if (!shortHdr) acPred = bget1(b);
        e = d->lutCbpy[bshow(b, CBPY_BITS)];
        if (!LUT_LEN(e)) return -1;
        bskip(b, LUT_LEN(e));
        cbpy = LUT_SYM(e);
        cbp = (cbpy << 2) | (mcbpc & 3);
        useDcVlc = d->qscale < d->intraDcThr;
        if (dquant) d->qscale = mp_clip(d->qscale + kDqTab[bget(b, 2)], 1, 31);
        d->mbIntra[mbi] = 1;
        d->mbQs[mbi] = (u8)d->qscale;
        mb_set_zero_mv(d, mx, my);
        for (i = 0; i < 6; i++)
            {
            int coded = (cbp >> (5 - i)) & 1;
            u8* dst = block_dst(d, i, mx, my, &stride);
            if (shortHdr)
                {
                int dc = bget(b, 8);
                if ((dc & 0x7F) == 0) return -1;
                if (dc == 255) dc = 128;
                if (coded && read_coeffs(d, b, d->blk, 1, kScanZigzag, 0, 1) < 0) return -1;
                d->blk[0] = dc * 8;
                dequant_intra_ac(d, d->blk);
                }
            else
                {
                if (decode_intra_block(d, b, i, coded, useDcVlc, acPred, mx, my) < 0) return -1;
                }
            idct_put(dst, stride, d->blk);
            }
        return 0;
        }

    /* inter */
    {
    u16 e = d->lutCbpy[bshow(b, CBPY_BITS)];
    if (!LUT_LEN(e)) return -1;
    bskip(b, LUT_LEN(e));
    cbpy = LUT_SYM(e) ^ 15;
    }
    cbp = (cbpy << 2) | (mcbpc & 3);
    if (dquant) d->qscale = mp_clip(d->qscale + kDqTab[bget(b, 2)], 1, 31);
    d->mbIntra[mbi] = 0;
    d->mbQs[mbi] = (u8)d->qscale;
    {
    int bw = 2 * d->mbw;
    int px, py, vx, vy;
    if (!four)
        {
        s16* m = d->mv + 2 * ((2 * my) * bw + 2 * mx);
        mv_pred(d, mx, my, 0, &px, &py);
        vx = decode_mv_comp(d, b, px, d->fcode);
        if (vx >= 0x10000) return -1;
        vy = decode_mv_comp(d, b, py, d->fcode);
        if (vy >= 0x10000) return -1;
        m[0] = m[2] = (s16)vx; m[1] = m[3] = (s16)vy;
        m += 2 * bw;
        m[0] = m[2] = (s16)vx; m[1] = m[3] = (s16)vy;
        }
    else
        {
        for (i = 0; i < 4; i++)
            {
            s16* m = d->mv + 2 * ((2 * my + (i >> 1)) * bw + 2 * mx + (i & 1));
            mv_pred(d, mx, my, i, &px, &py);
            vx = decode_mv_comp(d, b, px, d->fcode);
            if (vx >= 0x10000) return -1;
            vy = decode_mv_comp(d, b, py, d->fcode);
            if (vy >= 0x10000) return -1;
            m[0] = (s16)vx; m[1] = (s16)vy;
            }
        }
    }
    motion_comp(d, mx, my, four);
    for (i = 0; i < 6; i++)
        {
        if (!((cbp >> (5 - i)) & 1)) continue;
        if (read_coeffs(d, b, d->blk, 0, kScanZigzag, 0, shortHdr) < 0) return -1;
        dequant_inter(d, d->blk);
        {
        u8* dst = block_dst(d, i, mx, my, &stride);
        idct_add_rows(dst, stride, d->blk, d->nzRows);
        }
        }
    return 0;
    }

/* ------------------------------------------------------------------ picture level */

static void extend_plane(u8* p, int stride, int w, int h, int pad)
    {
    int y, x;
    for (y = 0; y < h; y++)
        {
        u8* row = p + y * stride;
        u8 l = row[0], r = row[w - 1];
        for (x = 1; x <= pad; x++) { row[-x] = l; row[w - 1 + x] = r; }
        }
    for (y = 1; y <= pad; y++)
        {
        mp_memcpy(p - y * stride - pad, p - pad, w + 2 * pad);
        mp_memcpy(p + (h - 1 + y) * stride - pad, p + (h - 1) * stride - pad, w + 2 * pad);
        }
    }

static void finish_picture(M4vDec* d, M4vPicture* pic)
    {
    Frame* f = &d->frm[d->cur];
    int mx, my, missing = 0;
    for (my = 0; my < d->mbh; my++)
        for (mx = 0; mx < d->mbw; mx++)
            if (d->mbSlice[my * d->mbw + mx] == 0xFFFF)
                {
                missing++;
                if (d->haveRef) copy_mb(d, &d->frm[d->cur ^ 1], f, mx, my);
                else
                    {
                    int y;
                    for (y = 0; y < 16; y++) mp_memset(f->y + (my * 16 + y) * d->ystride + mx * 16, 128, 16);
                    for (y = 0; y < 8; y++)
                        {
                        mp_memset(f->u + (my * 8 + y) * d->cstride + mx * 8, 128, 8);
                        mp_memset(f->v + (my * 8 + y) * d->cstride + mx * 8, 128, 8);
                        }
                    }
                }
    extend_plane(f->y, d->ystride, d->mbw * 16, d->mbh * 16, PAD);
    extend_plane(f->u, d->cstride, d->mbw * 8, d->mbh * 8, CPAD);
    extend_plane(f->v, d->cstride, d->mbw * 8, d->mbh * 8, CPAD);
    pic->y = f->y; pic->u = f->u; pic->v = f->v;
    pic->ystride = d->ystride; pic->cstride = d->cstride;
    pic->width = d->width; pic->height = d->height;
    /* the new picture becomes the reference */
    d->cur ^= 1;
    d->haveRef = 1;
    }

static void ref_picture(M4vDec* d, M4vPicture* pic)
    {
    const Frame* f = &d->frm[d->cur ^ 1];
    pic->y = f->y; pic->u = f->u; pic->v = f->v;
    pic->ystride = d->ystride; pic->cstride = d->cstride;
    pic->width = d->width; pic->height = d->height;
    }

/* MPEG-4: is a video packet resync marker next (after the stuffing)? Positions b after the stuffing. */
static int at_resync(M4vDec* d, Bits* b)
    {
    int s = 8 - (b->pos & 7);
    int prefix = (d->vopType == 0) ? 16 : 15 + d->fcode;
    Bits t;
    if (bshow(b, s) != (u32)((1 << (s - 1)) - 1)) return 0;
    t = *b;
    bskip(&t, s);
    if (bleft(&t) < prefix + 1) return 0;
    if (bshow(&t, prefix + 1) != 1) return 0;
    *b = t;
    return 1;
    }

/* parses a video packet header at a resync marker; returns the MB number or -1 */
static int video_packet_header(M4vDec* d, Bits* b)
    {
    int prefix = (d->vopType == 0) ? 16 : 15 + d->fcode;
    int mbBits = ilog2(d->mbnum - 1) + 1;
    int mbn, q;
    bskip(b, prefix + 1);
    mbn = bget(b, mbBits);
    if (mbn >= d->mbnum) return -1;
    q = bget(b, d->quantPrecision);
    if (q) d->qscale = q;
    if (bget1(b))                               /* header_extension_code */
        {
        int guard = 0;
        while (bget1(b) && guard++ < 32) {}
        bskip(b, 1);
        bskip(b, d->timeIncBits);
        bskip(b, 1);
        bskip(b, 2);
        bskip(b, 3);
        if (d->vopType != 0) bskip(b, 3);
        }
    return mbn;
    }

/* searches forward for the next resync marker (MPEG-4) or GOB start (H.263); -1 if none */
static int find_resync(M4vDec* d, Bits* b)
    {
    if (!d->shortHeader)
        {
        int prefix = (d->vopType == 0) ? 16 : 15 + d->fcode;
        balign(b);
        while (bleft(b) > prefix + 8)
            {
            if (bshow(b, prefix + 1) == 1) return 0;
            bskip(b, 8);
            }
        return -1;
        }
    while (bleft(b) > 17 + 12)
        {
        if (bshow(b, 17) == 1) return 0;
        bskip(b, 1);
        }
    return -1;
    }

static int decode_vop_mbs(M4vDec* d, Bits* b)
    {
    int mbi = 0, i;
    for (i = 0; i < d->mbnum; i++) d->mbSlice[i] = 0xFFFF;
    d->slice = 0;
    while (mbi < d->mbnum)
        {
        if (mbi > 0 && d->resyncMarker && at_resync(d, b))
            {
            int n = video_packet_header(d, b);
            if (n < 0) return 0;
            d->slice++;
            if (n > mbi) mbi = n;
            }
        if (bleft(b) < 0) return 0;
        if (decode_mb(d, b, mbi % d->mbw, mbi / d->mbw) < 0)
            {
            /* damaged: continue at the next video packet */
            int n;
            d->mbSlice[mbi] = 0xFFFF;
            mp_memset(d->blk, 0, sizeof(d->blk));
            if (!d->resyncMarker || find_resync(d, b) < 0) return 0;
            n = video_packet_header(d, b);
            if (n < 0) return 0;
            d->slice++;
            /* MBs between the error and n stay undecoded and are concealed */
            if (n > mbi) mbi = n;
            else mbi++;
            continue;
            }
        mbi++;
        }
    return 0;
    }

static int decode_vop(M4vDec* d, Bits* b, M4vPicture* pic)
    {
    int t;
    if (d->unsupported) return M4V_ERR_UNSUPPORTED;
    if (!d->haveVol) { set_err(d, "VOP before VOL header"); return M4V_ERR_BITSTREAM; }
    d->vopType = bget(b, 2);
    t = 0;
    while (bget1(b) && t++ < 64) {}             /* modulo_time_base */
    bskip(b, 1);
    bskip(b, d->timeIncBits);
    bskip(b, 1);
    if (!bget1(b))                              /* vop_coded */
        {
        if (!d->haveRef) return M4V_FRAME_SKIPPED;
        ref_picture(d, pic);
        return M4V_FRAME_REPEAT;
        }
    if (d->vopType == 2) return M4V_FRAME_SKIPPED;
    if (d->vopType == 3) { set_err(d, "S-VOP (GMC)"); return M4V_ERR_UNSUPPORTED; }
    if (d->vopType == 1 && !d->haveRef) return M4V_ERR_NOREF;
    d->rounding = (d->vopType == 1) ? (int)bget1(b) : 0;
    bskip(b, d->cplxTrashI);
    if (d->vopType == 1) bskip(b, d->cplxTrashP);
    d->intraDcThr = kDcThreshold[bget(b, 3)];
    d->qscale = bget(b, d->quantPrecision);
    if (d->qscale == 0) { set_err(d, "bad VOP header (quant)"); return M4V_ERR_BITSTREAM; }
    if (d->qscale > 31) d->qscale = 31;
    d->fcode = 1;
    if (d->vopType == 1)
        {
        d->fcode = bget(b, 3);
        if (d->fcode == 0) { set_err(d, "bad VOP header (fcode)"); return M4V_ERR_BITSTREAM; }
        }
    decode_vop_mbs(d, b);
    finish_picture(d, pic);
    return d->vopType == 0 ? M4V_FRAME_I : M4V_FRAME_P;
    }

/* ------------------------------------------------------------------ H.263 baseline */

static int gob_header(M4vDec* d, Bits* b, int* gn)
    {
    Bits t = *b;
    if (bshow(&t, 17) != 1)
        {
        int s = (8 - (t.pos & 7)) & 7;
        if (!s || bshow(&t, s) != 0) return 0;
        bskip(&t, s);
        if (bshow(&t, 17) != 1) return 0;
        }
    bskip(&t, 17);
    *gn = bget(&t, 5);
    bskip(&t, 2);                               /* GFID */
    d->qscale = bget(&t, 5);
    if (d->qscale == 0) return 0;
    *b = t;
    return 1;
    }

static const u16 kH263Sizes[6][2] = { { 0, 0 }, { 128, 96 }, { 176, 144 }, { 352, 288 }, { 704, 576 }, { 1408, 1152 } };

static int decode_h263(M4vDec* d, Bits* b, M4vPicture* pic)
    {
    int fmt, ptype, r, rowsPerGob, gobs, g, mbi;
    bskip(b, 22);                               /* PSC */
    bskip(b, 8);                                /* TR */
    if (bget1(b) != 1 || bget1(b) != 0) { set_err(d, "bad H.263 picture header"); return M4V_ERR_BITSTREAM; }
    bskip(b, 3);                                /* split screen, document camera, freeze release */
    fmt = bget(b, 3);
    if (fmt == 0 || fmt > 5) { set_err(d, "H.263+ (PLUSPTYPE) stream"); return M4V_ERR_UNSUPPORTED; }
    ptype = bget1(b);
    if (bget(b, 4)) { set_err(d, "H.263 optional modes (UMV/SAC/AP/PB)"); return M4V_ERR_UNSUPPORTED; }
    d->qscale = bget(b, 5);
    if (d->qscale == 0) d->qscale = 1;
    if (bget1(b)) bskip(b, 2);                  /* CPM, PSBI */
    { int guard = 0; while (bget1(b) && guard++ < 256) bskip(b, 8); }   /* PEI / PSPARE */

    if (!d->shortHeader || d->width != kH263Sizes[fmt][0] || d->height != kH263Sizes[fmt][1])
        {
        d->shortHeader = 1;
        d->quantType = 0;
        r = set_size(d, kH263Sizes[fmt][0], kH263Sizes[fmt][1]);
        if (r < 0) return r;
        }
    d->shortHeader = 1;
    d->haveVol = 1;
    d->vopType = ptype;
    d->rounding = 0;
    d->fcode = 1;
    d->intraDcThr = 0;
    if (ptype == 1 && !d->haveRef) return M4V_ERR_NOREF;

    rowsPerGob = fmt <= 3 ? 1 : (fmt == 4 ? 2 : 4);
    gobs = d->mbh / rowsPerGob;
    for (mbi = 0; mbi < d->mbnum; mbi++) d->mbSlice[mbi] = 0xFFFF;
    d->slice = 0;
    g = 0;
    while (g < gobs)
        {
        int ok = 1, k, first, count;
        if (g > 0)
            {
            int gn;
            if (gob_header(d, b, &gn))
                {
                if (gn >= gobs) break;          /* end of sequence or garbage */
                if (gn > g) g = gn;
                d->slice++;
                }
            }
        first = g * rowsPerGob * d->mbw;
        count = rowsPerGob * d->mbw;
        for (k = 0; k < count; k++)
            {
            int m = first + k;
            if (bleft(b) < 0 || decode_mb(d, b, m % d->mbw, m / d->mbw) < 0) { d->mbSlice[m] = 0xFFFF; mp_memset(d->blk, 0, sizeof(d->blk)); ok = 0; break; }
            }
        if (!ok)
            {
            /* damaged: look for the next GOB header */
            int gn;
            if (find_resync(d, b) < 0) break;
            if (!gob_header(d, b, &gn) || gn <= g || gn >= gobs) break;
            g = gn;
            d->slice++;
            /* decode from the found GOB (header already consumed) */
            first = g * rowsPerGob * d->mbw;
            for (k = 0; k < count; k++)
                {
                int m = first + k;
                if (bleft(b) < 0 || decode_mb(d, b, m % d->mbw, m / d->mbw) < 0) { d->mbSlice[m] = 0xFFFF; mp_memset(d->blk, 0, sizeof(d->blk)); break; }
                }
            }
        g++;
        }
    finish_picture(d, pic);
    return ptype ? M4V_FRAME_P : M4V_FRAME_I;
    }

/* ------------------------------------------------------------------ entry points */

/* finds the next 00 00 01 xx start code at or after byte offset i; returns offset of xx or -1 */
static int next_start(const u8* p, int len, int i)
    {
    for (; i + 3 < len; i++)
        if (p[i] == 0 && p[i + 1] == 0 && p[i + 2] == 1) return i + 3;
    return -1;
    }

static int parse_headers(M4vDec* d, const u8* buf, int len, M4vPicture* pic, int allowVop)
    {
    int i = 0, result = M4V_NOFRAME;
    while ((i = next_start(buf, len, i)) >= 0)
        {
        int code = buf[i];
        Bits b;
        b.buf = buf + i + 1;
        b.pos = 0;
        b.size = (len - i - 1) * 8;
        if (code >= 0x20 && code <= 0x2F)
            {
            int r = parse_vol(d, &b);
            if (r < 0) return r;
            }
        else if (code == 0xB6 && allowVop)
            {
            return decode_vop(d, &b, pic);
            }
        i++;
        }
    return result;
    }

int m4v_config(M4vDec* d, const u8* buf, int len)
    {
    M4vPicture dummy;
    int r;
    if (len >= 3 && buf[0] == 0 && buf[1] == 0 && (buf[2] & 0xFC) == 0x80) return 0;   /* H.263 */
    r = parse_headers(d, buf, len, &dummy, 0);
    return r < 0 ? r : 0;
    }

int m4v_decode(M4vDec* d, const u8* buf, int len, M4vPicture* pic)
    {
    if (len < 4) return M4V_NOFRAME;
    if (buf[0] == 0 && buf[1] == 0 && (buf[2] & 0xFC) == 0x80)
        {
        Bits b;
        b.buf = buf; b.pos = 0; b.size = len * 8;
        return decode_h263(d, &b, pic);
        }
    return parse_headers(d, buf, len, pic, 1);
    }

u32 m4v_picture_hash(const M4vPicture* p, u32 h)
    {
    int x, y, cw = (p->width + 1) >> 1, ch = (p->height + 1) >> 1;
    for (y = 0; y < p->height; y++)
        {
        const u8* r = p->y + y * p->ystride;
        for (x = 0; x < p->width; x++) h = (h ^ r[x]) * 16777619u;
        }
    for (y = 0; y < ch; y++)
        {
        const u8* ru = p->u + y * p->cstride;
        const u8* rv = p->v + y * p->cstride;
        for (x = 0; x < cw; x++) h = (h ^ ru[x]) * 16777619u;
        for (x = 0; x < cw; x++) h = (h ^ rv[x]) * 16777619u;
        }
    return h;
    }
