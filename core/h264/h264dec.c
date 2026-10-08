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
    };

int h264d_open(H264Dec** aDec)
    {
    H264Dec* d;
    *aDec = NULL;
    d = (H264Dec*)malloc(sizeof(H264Dec));
    if (!d) return H264D_ERR_NOMEM;
    memset(d, 0, sizeof(H264Dec));
    d->nalLen = 4;
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

void h264d_set_spare(H264Dec* d, int count, int budget)
    {
    if (!d) return;
    d->st->dpb->spareWanted = count < 0 ? 0 : (u32)count;
    d->st->dpb->spareBudget = budget < 0 ? 0 : (u32)budget;
    }

int h264d_spare(H264Dec* d)
    {
    return d ? (int)d->st->dpb->numSpare : 0;
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
        feed_nal(d, avcc + pos, (u32)l, &dummy, &got);
        pos += l;
        }
    /* a picture parameter set that didn't decode means CABAC (Main profile) or another unsupported tool */
    if (d->errors != errors0) return H264D_ERR_UNSUPPORTED;
    return d->nalLen;
    }

int h264d_decode(H264Dec* d, unsigned char* buf, int len, H264Pic* pic)
    {
    int pos = 0, got = 0, r;
    while (pos + d->nalLen <= len)
        {
        u32 l = 0;
        int k;
        for (k = 0; k < d->nalLen; k++) l = (l << 8) | buf[pos + k];
        pos += d->nalLen;
        if (l > (u32)(len - pos)) { d->errors++; break; }
        r = feed_nal(d, buf + pos, l, pic, &got);
        if (r < 0) return r;
        pos += (int)l;
        }
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
