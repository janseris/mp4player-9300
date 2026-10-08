/*
 * MP4 Player 9300: H.264 decoder for MP4 'avc1' tracks, on top of h264bsd.
 */
#include "basetype.h"
#include "h264bsd_decoder.h"
#include "h264bsd_util.h"
#include "h264dec.h"

struct H264Dec
    {
    storage_t* st;
    int nalLen;         /* bytes of the NAL length field */
    int profile;
    int errors;
    u32 picId;
    int fast;
    int cabac;
    H264Stats stats;
    };

/* tiny bit reader for the start of a NAL unit (no emulation prevention handling: only a
   few header bits are read) */
typedef struct { const u8* p; int len; int pos; } MiniBits;

static int mb_bit(MiniBits* b)
    {
    int v;
    if ((b->pos >> 3) >= b->len) return -1;
    v = (b->p[b->pos >> 3] >> (7 - (b->pos & 7))) & 1;
    b->pos++;
    return v;
    }

static int mb_ue(MiniBits* b)
    {
    int z = 0, v = 0, i, bit;
    while ((bit = mb_bit(b)) == 0) if (++z > 24) return -1;
    if (bit < 0) return -1;
    for (i = 0; i < z; i++)
        {
        bit = mb_bit(b);
        if (bit < 0) return -1;
        v = (v << 1) | bit;
        }
    return (1 << z) - 1 + v;
    }

int h264d_open(H264Dec** aDec)
    {
    H264Dec* d;
    *aDec = NULL;
    d = (H264Dec*)malloc(sizeof(H264Dec));
    if (!d) return H264D_ERR_NOMEM;
    memset(d, 0, sizeof(H264Dec));
    d->nalLen = 4;
    d->cabac = -1;
    d->st = h264bsdAlloc();
    if (!d->st) { free(d); return H264D_ERR_NOMEM; }
    memset(d->st, 0, sizeof(storage_t));
    if (h264bsdInit(d->st, HANTRO_TRUE) != HANTRO_OK)
        {
        h264bsdFree(d->st);
        free(d);
        return H264D_ERR_NOMEM;
        }
    *aDec = d;
    return 0;
    }

void h264d_set_fast(H264Dec* d, int fast)
    {
    if (d) d->fast = fast ? 1 : 0;
    }

void h264d_close(H264Dec* d)
    {
    if (!d) return;
    h264bsdShutdown(d->st);
    h264bsdFree(d->st);
    free(d);
    }

/* feeds one NAL unit (without length / start code) */
static int feed_nal(H264Dec* d, u8* nal, u32 len, H264Pic* pic, int* got)
    {
    u32 used, guard = 0;
    u8* p = nal;
    if (len == 0) return 0;
    if (d->profile == 0 && (nal[0] & 0x1F) == 7 && len > 1) d->profile = nal[1];
    while (len > 0 && guard++ < 4)
        {
        u32 r;
        d->st->skipDeblocking = (u32)d->fast;
        r = h264bsdDecode(d->st, p, len, d->picId, &used);
        if ((r == H264BSD_RDY || r == H264BSD_PIC_RDY) && used == len && p == nal &&
            ((nal[0] & 0x1F) == 1 || (nal[0] & 0x1F) == 5))
            {
            sliceHeader_t* sh = d->st->sliceHeader;
            d->stats.decoded++;
            if (d->st->activePps) d->stats.qpSum += (int)d->st->activePps->picInitQp + sh->sliceQpDelta;
            if (sh->disableDeblockingFilterIdc == 1) d->stats.deblockOff++;
            }
        if (r == H264BSD_PIC_RDY)
            {
            u32 id, idr, err, crop, left, w, top, h;
            u8* data;
            u8* last = NULL;
            u32 lastIdr = 0;
            d->picId++;
            /* no reordering: the picture comes out straight away (take the newest) */
            while ((data = h264bsdNextOutputPicture(d->st, &id, &idr, &err)) != NULL)
                {
                last = data;
                lastIdr = idr;
                }
            if (last)
                {
                u32 W = h264bsdPicWidth(d->st) * 16, H = h264bsdPicHeight(d->st) * 16;
                h264bsdCroppingParams(d->st, &crop, &left, &w, &top, &h);
                if (!crop) { left = 0; top = 0; w = W; h = H; }
                pic->ystride = (int)W;
                pic->cstride = (int)(W / 2);
                pic->y = last + top * W + left;
                pic->u = last + W * H + (top / 2) * (W / 2) + left / 2;
                pic->v = last + W * H + (W / 2) * (H / 2) + (top / 2) * (W / 2) + left / 2;
                pic->width = (int)w;
                pic->height = (int)h;
                *got = lastIdr ? 2 : 1;
                }
            }
        else if (r == H264BSD_MEMALLOC_ERROR)
            return H264D_ERR_NOMEM;
        else if (r == H264BSD_ERROR || r == H264BSD_PARAM_SET_ERROR)
            {
            d->errors++;
            return 0;   /* skip this NAL unit */
            }
        if (used == 0 && r != H264BSD_HDRS_RDY && r != H264BSD_PIC_RDY) break;
        p += used;
        len -= used;
        }
    return 0;
    }

int h264d_config(H264Dec* d, unsigned char* avcc, int len)
    {
    int i, n, pos, got = 0;
    H264Pic dummy;
    int errors0 = d->errors;
    if (len < 7 || avcc[0] != 1) return H264D_ERR_GENERIC;
    d->profile = avcc[1];
    /* High profiles have a different SPS syntax (and use CABAC / 8x8 transforms): not decodable */
    if (d->profile >= 100) return H264D_ERR_UNSUPPORTED;
    d->nalLen = (avcc[4] & 3) + 1;
    if (d->nalLen == 3) return H264D_ERR_UNSUPPORTED;
    n = avcc[5] & 0x1F;
    pos = 6;
    for (i = 0; i < n; i++)
        {
        int l;
        if (pos + 2 > len) return H264D_ERR_GENERIC;
        l = (avcc[pos] << 8) | avcc[pos + 1];
        pos += 2;
        if (pos + l > len) return H264D_ERR_GENERIC;
        feed_nal(d, avcc + pos, (u32)l, &dummy, &got);
        pos += l;
        }
    if (pos >= len) return d->nalLen;
    n = avcc[pos++];
    for (i = 0; i < n; i++)
        {
        int l;
        if (pos + 2 > len) return H264D_ERR_GENERIC;
        l = (avcc[pos] << 8) | avcc[pos + 1];
        pos += 2;
        if (pos + l > len) return H264D_ERR_GENERIC;
        if (i == 0 && l > 1)
            {
            MiniBits b;
            b.p = avcc + pos + 1; b.len = l - 1; b.pos = 0;
            mb_ue(&b);                          /* pps id */
            mb_ue(&b);                          /* sps id */
            d->cabac = mb_bit(&b);              /* entropy_coding_mode_flag */
            }
        feed_nal(d, avcc + pos, (u32)l, &dummy, &got);
        pos += l;
        }
    /* a picture parameter set that didn't decode means CABAC (Main profile) or another unsupported tool */
    if (d->errors != errors0) return H264D_ERR_UNSUPPORTED;
    return d->nalLen;
    }

int h264d_decode(H264Dec* d, unsigned char* buf, int len, H264Pic* pic)
    {
    int pos = 0, got = 0, r, slices = 0;
    while (pos + d->nalLen <= len)
        {
        u32 l = 0;
        int k;
        for (k = 0; k < d->nalLen; k++) l = (l << 8) | buf[pos + k];
        pos += d->nalLen;
        if (l > (u32)(len - pos)) { d->errors++; break; }
        if (l > 0)
            {
            int t = buf[pos] & 0x1F;
            d->stats.nals++;
            if (t == 1 || t == 5)
                {
                MiniBits b;
                int st;
                b.p = buf + pos + 1; b.len = (int)l - 1; b.pos = 0;
                mb_ue(&b);                      /* first_mb_in_slice */
                st = mb_ue(&b);                 /* slice_type */
                if (st >= 5) st -= 5;
                d->stats.slices++;
                slices++;
                if (t == 5) d->stats.idrSlices++;
                if (st == 2) d->stats.iSlices++;
                else if (st == 0) d->stats.pSlices++;
                else if (st == 1) d->stats.bSlices++;
                else d->stats.otherSlices++;
                }
            else if (t == 6) d->stats.sei++;
            else if (t == 9) d->stats.aud++;
            else if (t == 7 || t == 8) d->stats.paramSets++;
            else d->stats.other++;
            }
        r = feed_nal(d, buf + pos, l, pic, &got);
        if (r < 0) return r;
        pos += (int)l;
        }
    if (slices > d->stats.maxSlicesPerSample) d->stats.maxSlicesPerSample = slices;
    if (got == 2) return H264D_FRAME_KEY;
    if (got == 1) return H264D_FRAME;
    return H264D_NOFRAME;
    }

void h264d_flush(H264Dec* d)
    {
    h264bsdFlushBuffer(d->st);
    }

int h264d_profile(H264Dec* d) { return d->profile; }
int h264d_errors(H264Dec* d) { return d->errors; }

void h264d_stats(H264Dec* d, H264Stats* s) { *s = d->stats; }

void h264d_params(H264Dec* d, H264Params* p)
    {
    seqParamSet_t* sps;
    picParamSet_t* pps;
    int i;
    memset(p, 0, sizeof(H264Params));
    p->cabac = d->cabac;
    p->nalLen = d->nalLen;
    p->profile = d->profile;
    sps = d->st->activeSps;
    for (i = 0; !sps && i < MAX_NUM_SEQ_PARAM_SETS; i++) sps = d->st->sps[i];
    pps = d->st->activePps;
    for (i = 0; !pps && i < MAX_NUM_PIC_PARAM_SETS; i++) pps = d->st->pps[i];
    if (sps)
        {
        u32 m = sps->maxFrameNum;
        p->valid = 1;
        p->profile = (int)sps->profileIdc;
        p->level = (int)sps->levelIdc;
        p->refs = (int)sps->numRefFrames;
        p->pocType = (int)sps->picOrderCntType;
        p->mbW = (int)sps->picWidthInMbs;
        p->mbH = (int)sps->picHeightInMbs;
        while (m > 1) { p->frameNumBits++; m >>= 1; }
        p->crop = (int)sps->frameCroppingFlag;
        p->cropL = (int)sps->frameCropLeftOffset * 2;
        p->cropR = (int)sps->frameCropRightOffset * 2;
        p->cropT = (int)sps->frameCropTopOffset * 2;
        p->cropB = (int)sps->frameCropBottomOffset * 2;
        p->vui = (int)sps->vuiParametersPresentFlag;
        if (sps->vuiParametersPresentFlag && sps->vuiParameters)
            {
            vuiParameters_t* v = sps->vuiParameters;
            p->timing = (int)v->timingInfoPresentFlag;
            p->numUnitsInTick = (int)v->numUnitsInTick;
            p->timeScale = (int)v->timeScale;
            p->fixedRate = (int)v->fixedFrameRateFlag;
            p->restrict_ = (int)v->bitstreamRestrictionFlag;
            p->reorder = (int)v->numReorderFrames;
            p->maxDecBuf = (int)v->maxDecFrameBuffering;
            }
        }
    if (pps)
        {
        p->ppsValid = 1;
        p->sliceGroups = (int)pps->numSliceGroups;
        p->refIdx = (int)pps->numRefIdxL0Active;
        p->initQp = (int)pps->picInitQp;
        p->chromaQpOff = (int)pps->chromaQpIndexOffset;
        p->deblockCtrl = (int)pps->deblockingFilterControlPresentFlag;
        p->constrIntra = (int)pps->constrainedIntraPredFlag;
        p->redundant = (int)pps->redundantPicCntPresentFlag;
        }
    }
