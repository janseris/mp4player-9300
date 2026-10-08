#include "viddec.h"
#include "h264/h264dec.h"

struct VidDec
    {
    MpAlloc al;
    M4vDec* m4v;
    H264Dec* h264;
    int width, height;
    char err[64];
    char info[48];
    };

static void copy_text(char* dst, int cap, const char* src)
    {
    int i = 0;
    while (src[i] && i < cap - 1) { dst[i] = src[i]; i++; }
    dst[i] = 0;
    }

static void append_int(char* dst, int cap, int v)
    {
    char t[12];
    int n = 0, k = 0;
    while (dst[n]) n++;
    do { t[k++] = (char)('0' + v % 10); v /= 10; } while (v && k < 11);
    while (k > 0 && n < cap - 1) dst[n++] = t[--k];
    dst[n] = 0;
    }

int viddec_open(VidDec** aDec, const MpAlloc* aAlloc, const Mp4VideoTrack* t)
    {
    VidDec* d;
    int r;
    *aDec = 0;
    d = (VidDec*)mp_alloc(aAlloc, sizeof(VidDec));
    if (!d) return M4V_ERR_NOMEM;
    d->al = *aAlloc;
    d->width = t->width;
    d->height = t->height;
    *aDec = d;
    if (t->codec == MP4_VCODEC_MPEG4 || t->codec == MP4_VCODEC_H263)
        {
        r = m4v_open(&d->m4v, aAlloc);
        if (r < 0) return r;
        if (t->dsiLen > 0)
            {
            r = m4v_config(d->m4v, t->dsi, t->dsiLen);
            if (r == M4V_ERR_UNSUPPORTED) { copy_text(d->err, sizeof(d->err), m4v_error_text(d->m4v)); return r; }
            }
        return 0;
        }
    if (t->codec == MP4_VCODEC_H264)
        {
        u8* cfg;
        if (h264d_open(&d->h264) < 0) return M4V_ERR_NOMEM;
        if (t->dsiLen <= 0) { copy_text(d->err, sizeof(d->err), "no avcC box"); return M4V_ERR_UNSUPPORTED; }
        /* h264bsd modifies its input: work on a copy */
        cfg = (u8*)mp_alloc(aAlloc, t->dsiLen + MP_INPUT_PADDING);
        if (!cfg) return M4V_ERR_NOMEM;
        mp_memcpy(cfg, t->dsi, t->dsiLen);
        r = h264d_config(d->h264, cfg, t->dsiLen);
        mp_free(aAlloc, cfg);
        if (r == H264D_ERR_UNSUPPORTED)
            {
            int prof = h264d_profile(d->h264);
            if (prof >= 100) copy_text(d->err, sizeof(d->err), "H.264 High profile (only Baseline plays)");
            else copy_text(d->err, sizeof(d->err), "H.264 Main profile with CABAC (only Baseline plays)");
            return M4V_ERR_UNSUPPORTED;
            }
        if (r < 0) { copy_text(d->err, sizeof(d->err), "bad avcC box"); return M4V_ERR_UNSUPPORTED; }
        r = h264d_profile(d->h264);
        if (r != 66 && r != 0)
            {
            /* Main/High: decodable only without CABAC, B-frames and interlace */
            copy_text(d->info, sizeof(d->info), "H.264 profile ");
            append_int(d->info, sizeof(d->info), r);
            }
        else copy_text(d->info, sizeof(d->info), "H.264 Baseline");
        return 0;
        }
    copy_text(d->err, sizeof(d->err), "unknown video codec");
    return M4V_ERR_UNSUPPORTED;
    }

void viddec_close(VidDec* d)
    {
    MpAlloc al;
    if (!d) return;
    al = d->al;
    if (d->m4v) m4v_close(d->m4v);
    if (d->h264) h264d_close(d->h264);
    mp_free(&al, d);
    }

int viddec_decode(VidDec* d, u8* buf, int len, M4vPicture* pic)
    {
    if (d->m4v) return m4v_decode(d->m4v, buf, len, pic);
    if (d->h264)
        {
        H264Pic hp;
        int r = h264d_decode(d->h264, buf, len, &hp);
        if (r == H264D_FRAME_KEY || r == H264D_FRAME)
            {
            pic->y = hp.y; pic->u = hp.u; pic->v = hp.v;
            pic->ystride = hp.ystride; pic->cstride = hp.cstride;
            pic->width = hp.width; pic->height = hp.height;
            return r == H264D_FRAME_KEY ? M4V_FRAME_I : M4V_FRAME_P;
            }
        if (r == H264D_ERR_NOMEM) { copy_text(d->err, sizeof(d->err), "out of memory"); return M4V_ERR_NOMEM; }
        return r < 0 ? (int)M4V_ERR_BITSTREAM : (int)M4V_NOFRAME;
        }
    return M4V_ERR_GENERIC;
    }

void viddec_flush(VidDec* d)
    {
    if (d->m4v) m4v_flush(d->m4v);
    if (d->h264) h264d_flush(d->h264);
    }

const char* viddec_error_text(VidDec* d)
    {
    if (d->err[0]) return d->err;
    if (d->m4v) return m4v_error_text(d->m4v);
    return d->err;
    }

const char* viddec_stream_info(VidDec* d)
    {
    if (d->m4v) return m4v_stream_info(d->m4v);
    if (d->h264 && d->info[0] == 0) copy_text(d->info, sizeof(d->info), "H.264");
    return d->info;
    }

int viddec_is_h264(VidDec* d) { return d->h264 != 0; }
void viddec_set_fast(VidDec* d, int fast) { if (d && d->h264) h264d_set_fast(d->h264, fast); }

int viddec_h264_info(VidDec* d, H264Params* p, H264Stats* s)
    {
    if (!d || !d->h264) return 0;
    h264d_params(d->h264, p);
    h264d_stats(d->h264, s);
    return 1;
    }

int viddec_m4v_info(VidDec* d, M4vParams* p)
    {
    if (!d || !d->m4v) return 0;
    m4v_params(d->m4v, p);
    return 1;
    }
