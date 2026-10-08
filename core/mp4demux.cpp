#include "mp4demux.h"

#define MAX_MOOV (16 * 1024 * 1024)

static u32 rd32(const u8* p) { return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3]; }
static u32 rd16(const u8* p) { return ((u32)p[0] << 8) | p[1]; }

static void seterr(Mp4File* f, const char* s)
    {
    int i = 0;
    while (s[i] && i < (int)sizeof(f->err) - 1) { f->err[i] = s[i]; i++; }
    f->err[i] = 0;
    }

/* converts a time in track units to milliseconds without 64-bit arithmetic */
static u32 to_ms(u32 t, u32 scale)
    {
    if (scale == 0) return 0;
    return (t / scale) * 1000 + ((t % scale) * 1000) / scale;
    }

struct Span { const u8* p; u32 len; };

/* table boxes of one trak (pointers into the moov buffer) */
struct TrakInfo
    {
    u32 handler;
    u32 timescale;
    u32 duration;
    Span stsd, stts, ctts, stsc, stsz, stz2, stco, co64, stss;
    };

/* finds child box 'type' inside span s (after skipping 'skip' header bytes) */
static int find_box(Span s, u32 type, Span* out)
    {
    u32 pos = 0;
    while (pos + 8 <= s.len)
        {
        u32 size = rd32(s.p + pos);
        u32 t = rd32(s.p + pos + 4);
        u32 hdr = 8;
        if (size == 1)
            {
            if (pos + 16 > s.len) return 0;
            if (rd32(s.p + pos + 8) != 0) return 0;
            size = rd32(s.p + pos + 12);
            hdr = 16;
            }
        else if (size == 0) size = s.len - pos;
        if (size < hdr || pos + size > s.len) return 0;
        if (t == type)
            {
            out->p = s.p + pos + hdr;
            out->len = size - hdr;
            return 1;
            }
        pos += size;
        }
    return 0;
    }

/* descriptor length (expandable size) */
static int desc_len(const u8* p, u32 avail, u32* used)
    {
    u32 n = 0, i = 0;
    while (i < 4 && i < avail)
        {
        u8 c = p[i++];
        n = (n << 7) | (c & 0x7F);
        if (!(c & 0x80)) break;
        }
    *used = i;
    return (int)n;
    }

/* parses an esds box payload: object type and decoder specific info */
static void parse_esds(Span e, int* objType, const u8** dsi, int* dsiLen)
    {
    const u8* p = e.p + 4;          /* version/flags */
    const u8* end = e.p + e.len;
    u32 used;
    int len;
    *objType = 0; *dsi = 0; *dsiLen = 0;
    if (e.len < 6) return;
    if (*p != 3) return;
    p++;
    len = desc_len(p, end - p, &used); p += used;
    if (p + 3 > end) return;
    {
    int flags = p[2];
    p += 3;
    if (flags & 0x80) p += 2;
    if (flags & 0x40) { if (p >= end) return; p += 1 + p[0]; }
    if (flags & 0x20) p += 2;
    }
    if (p >= end || *p != 4) return;
    p++;
    len = desc_len(p, end - p, &used); p += used;
    if (p + 13 > end) return;
    *objType = p[0];
    p += 13;
    if (p < end && *p == 5)
        {
        p++;
        len = desc_len(p, end - p, &used); p += used;
        if (len > 0 && p + len <= end) { *dsi = p; *dsiLen = len; }
        }
    }

static void parse_trak(Span trak, TrakInfo* ti)
    {
    Span mdia, minf, stbl, b;
    mp_memset(ti, 0, sizeof(TrakInfo));
    if (!find_box(trak, MP4_FOURCC('m','d','i','a'), &mdia)) return;
    if (find_box(mdia, MP4_FOURCC('h','d','l','r'), &b) && b.len >= 12) ti->handler = rd32(b.p + 8);
    if (find_box(mdia, MP4_FOURCC('m','d','h','d'), &b) && b.len >= 24)
        {
        if (b.p[0] == 1)
            {
            if (b.len >= 36)
                {
                ti->timescale = rd32(b.p + 20);
                ti->duration = rd32(b.p + 24) ? 0xFFFFFFFF : rd32(b.p + 28);
                }
            }
        else
            {
            ti->timescale = rd32(b.p + 12);
            ti->duration = rd32(b.p + 16);
            }
        }
    if (!find_box(mdia, MP4_FOURCC('m','i','n','f'), &minf)) return;
    if (!find_box(minf, MP4_FOURCC('s','t','b','l'), &stbl)) return;
    find_box(stbl, MP4_FOURCC('s','t','s','d'), &ti->stsd);
    find_box(stbl, MP4_FOURCC('s','t','t','s'), &ti->stts);
    find_box(stbl, MP4_FOURCC('c','t','t','s'), &ti->ctts);
    find_box(stbl, MP4_FOURCC('s','t','s','c'), &ti->stsc);
    find_box(stbl, MP4_FOURCC('s','t','s','z'), &ti->stsz);
    find_box(stbl, MP4_FOURCC('s','t','z','2'), &ti->stz2);
    find_box(stbl, MP4_FOURCC('s','t','c','o'), &ti->stco);
    find_box(stbl, MP4_FOURCC('c','o','6','4'), &ti->co64);
    find_box(stbl, MP4_FOURCC('s','t','s','s'), &ti->stss);
    }

static void parse_audio(Mp4File* f, const TrakInfo* ti)
    {
    Span s = ti->stsd;
    u32 esize;
    if (f->audio.present || s.len < 16) return;
    esize = rd32(s.p + 8);
    if (esize < 36 || 8 + esize > s.len) return;
    f->audio.present = 1;
    f->audio.fourcc = rd32(s.p + 12);
    f->audio.channels = (int)rd16(s.p + 8 + 24);
    f->audio.sampleRate = (int)rd16(s.p + 8 + 32);
    f->audio.durationMs = to_ms(ti->duration, ti->timescale);
    if (f->audio.fourcc == MP4_FOURCC('m','p','4','a'))
        {
        Span entry, esds;
        const u8* dsi;
        int dl;
        u32 skip = 36;
        int ver = (int)rd16(s.p + 8 + 16);
        if (ver == 1) skip += 16;
        else if (ver == 2) skip += 36;
        if (skip < esize)
            {
            entry.p = s.p + 8 + skip;
            entry.len = esize - skip;
            if (find_box(entry, MP4_FOURCC('e','s','d','s'), &esds))
                {
                parse_esds(esds, &f->audio.objectType, &dsi, &dl);
                if (dsi && dl > 0)
                    {
                    f->audio.ascLen = dl > 8 ? 8 : dl;
                    mp_memcpy(f->audio.asc, dsi, f->audio.ascLen);
                    }
                }
            }
        }
    if (ti->stsz.len >= 12)
        {
        u32 fixed = rd32(ti->stsz.p + 4), n = rd32(ti->stsz.p + 8), i, sum = 0;
        f->audio.samples = n;
        if (fixed) sum = fixed * n;
        else
            {
            if (n > (ti->stsz.len - 12) / 4) n = (ti->stsz.len - 12) / 4;
            for (i = 0; i < n; i++) sum += rd32(ti->stsz.p + 12 + 4 * i);
            }
        f->audio.totalBytes = sum;
        }
    }

static int build_video(Mp4File* f, const TrakInfo* ti)
    {
    Mp4VideoTrack* v = &f->video;
    Span s = ti->stsd;
    u32 esize, n, i, k;
    const MpAlloc* al = &f->al;

    if (s.len < 16) return MP4_ERR_FORMAT;
    esize = rd32(s.p + 8);
    if (esize < 86 || 8 + esize > s.len) return MP4_ERR_FORMAT;
    v->fourcc = rd32(s.p + 12);
    v->width = (int)rd16(s.p + 8 + 32);
    v->height = (int)rd16(s.p + 8 + 34);
    v->timescale = ti->timescale ? ti->timescale : 1000;
    v->durationMs = to_ms(ti->duration, v->timescale);
    {
    Span entry, ch;
    entry.p = s.p + 8 + 86;
    entry.len = esize - 86;
    if (v->fourcc == MP4_FOURCC('m','p','4','v'))
        {
        int obj = 0, dl = 0;
        const u8* dsi = 0;
        v->codec = MP4_VCODEC_MPEG4;
        if (find_box(entry, MP4_FOURCC('e','s','d','s'), &ch))
            {
            parse_esds(ch, &obj, &dsi, &dl);
            if (obj == 0x21) v->codec = MP4_VCODEC_H264;
            else if (obj == 0x6A || obj == 0x60 || obj == 0x61 || obj == 0x6C) v->codec = MP4_VCODEC_OTHER;
            if (dl > 0)
                {
                v->dsi = (u8*)mp_alloc(al, dl + MP_INPUT_PADDING);
                if (!v->dsi) return MP4_ERR_NOMEM;
                mp_memcpy(v->dsi, dsi, dl);
                v->dsiLen = dl;
                }
            }
        }
    else if (v->fourcc == MP4_FOURCC('s','2','6','3') || v->fourcc == MP4_FOURCC('h','2','6','3') ||
             v->fourcc == MP4_FOURCC('H','2','6','3'))
        v->codec = MP4_VCODEC_H263;
    else if (v->fourcc == MP4_FOURCC('a','v','c','1') || v->fourcc == MP4_FOURCC('a','v','c','3'))
        {
        /* the AVCDecoderConfigurationRecord (SPS, PPS, NAL length size) becomes the dsi */
        v->codec = MP4_VCODEC_H264;
        if (find_box(entry, MP4_FOURCC('a','v','c','C'), &ch) && ch.len >= 7)
            {
            v->dsi = (u8*)mp_alloc(al, (int)ch.len + MP_INPUT_PADDING);
            if (!v->dsi) return MP4_ERR_NOMEM;
            mp_memcpy(v->dsi, ch.p, (int)ch.len);
            v->dsiLen = (int)ch.len;
            }
        }
    else
        v->codec = MP4_VCODEC_OTHER;
    }

    /* sample sizes */
    if (ti->stsz.len >= 12)
        {
        u32 fixed = rd32(ti->stsz.p + 4);
        n = rd32(ti->stsz.p + 8);
        if (n > 4000000) return MP4_ERR_FORMAT;
        if (!fixed && ti->stsz.len < 12 + 4 * n) return MP4_ERR_FORMAT;
        v->count = (int)n;
        v->offset = (u32*)mp_alloc(al, 4 * n);
        v->size = (u32*)mp_alloc(al, 4 * n);
        v->timeMs = (u32*)mp_alloc(al, 4 * n);
        v->key = (u8*)mp_alloc(al, n);
        if (!v->offset || !v->size || !v->timeMs || !v->key) return MP4_ERR_NOMEM;
        for (i = 0; i < n; i++) v->size[i] = fixed ? fixed : rd32(ti->stsz.p + 12 + 4 * i);
        }
    else if (ti->stz2.len >= 12)
        {
        int field = ti->stz2.p[7];
        n = rd32(ti->stz2.p + 8);
        if (n > 4000000 || (field != 4 && field != 8 && field != 16)) return MP4_ERR_FORMAT;
        if (ti->stz2.len < 12 + (n * field + 7) / 8) return MP4_ERR_FORMAT;
        v->count = (int)n;
        v->offset = (u32*)mp_alloc(al, 4 * n);
        v->size = (u32*)mp_alloc(al, 4 * n);
        v->timeMs = (u32*)mp_alloc(al, 4 * n);
        v->key = (u8*)mp_alloc(al, n);
        if (!v->offset || !v->size || !v->timeMs || !v->key) return MP4_ERR_NOMEM;
        for (i = 0; i < n; i++)
            {
            const u8* q = ti->stz2.p + 12;
            if (field == 16) v->size[i] = rd16(q + 2 * i);
            else if (field == 8) v->size[i] = q[i];
            else v->size[i] = (i & 1) ? (q[i / 2] & 15) : (q[i / 2] >> 4);
            }
        }
    else return MP4_ERR_FORMAT;
    n = (u32)v->count;
    if (n == 0) return MP4_OK;

    /* chunk offsets + sample-to-chunk -> sample offsets */
    {
    u32 nchunks, nstsc, e, sample = 0;
    int is64 = 0;
    if (ti->stco.len >= 8) nchunks = rd32(ti->stco.p + 4);
    else if (ti->co64.len >= 8) { nchunks = rd32(ti->co64.p + 4); is64 = 1; }
    else return MP4_ERR_FORMAT;
    if ((!is64 && nchunks > (ti->stco.len - 8) / 4) || (is64 && nchunks > (ti->co64.len - 8) / 8)) return MP4_ERR_FORMAT;
    if (ti->stsc.len < 8) return MP4_ERR_FORMAT;
    nstsc = rd32(ti->stsc.p + 4);
    if (nstsc > (ti->stsc.len - 8) / 12 || nstsc == 0) return MP4_ERR_FORMAT;
    for (e = 0; e < nstsc && sample < n; e++)
        {
        u32 first = rd32(ti->stsc.p + 8 + 12 * e);
        u32 per = rd32(ti->stsc.p + 8 + 12 * e + 4);
        u32 lastChunk = (e + 1 < nstsc) ? rd32(ti->stsc.p + 8 + 12 * (e + 1)) : nchunks + 1;
        u32 c;
        if (first == 0) first = 1;
        for (c = first; c < lastChunk && c <= nchunks && sample < n; c++)
            {
            u32 off;
            if (is64)
                {
                if (rd32(ti->co64.p + 8 + 8 * (c - 1)) != 0) { seterr(f, "file larger than 4 GB"); return MP4_ERR_FORMAT; }
                off = rd32(ti->co64.p + 8 + 8 * (c - 1) + 4);
                }
            else off = rd32(ti->stco.p + 8 + 4 * (c - 1));
            for (k = 0; k < per && sample < n; k++)
                {
                v->offset[sample] = off;
                off += v->size[sample];
                sample++;
                }
            }
        }
    if (sample < n) v->count = (int)(n = sample);
    }

    /* decoding times (+ composition offsets) */
    {
    u32 sample = 0, t = 0, e, cnt, delta;
    if (ti->stts.len >= 8)
        {
        u32 ne = rd32(ti->stts.p + 4);
        if (ne > (ti->stts.len - 8) / 8) ne = (ti->stts.len - 8) / 8;
        for (e = 0; e < ne && sample < n; e++)
            {
            cnt = rd32(ti->stts.p + 8 + 8 * e);
            delta = rd32(ti->stts.p + 8 + 8 * e + 4);
            for (k = 0; k < cnt && sample < n; k++) { v->timeMs[sample++] = t; t += delta; }
            }
        }
    for (; sample < n; sample++) v->timeMs[sample] = t;
    if (ti->ctts.len >= 8)
        {
        u32 ne = rd32(ti->ctts.p + 4);
        sample = 0;
        if (ne > (ti->ctts.len - 8) / 8) ne = (ti->ctts.len - 8) / 8;
        for (e = 0; e < ne && sample < n; e++)
            {
            cnt = rd32(ti->ctts.p + 8 + 8 * e);
            delta = rd32(ti->ctts.p + 8 + 8 * e + 4);
            for (k = 0; k < cnt && sample < n; k++, sample++)
                {
                s32 off = (s32)delta;
                s32 tt = (s32)v->timeMs[sample] + off;
                v->timeMs[sample] = tt < 0 ? 0 : (u32)tt;
                }
            }
        }
    for (sample = 0; sample < n; sample++) v->timeMs[sample] = to_ms(v->timeMs[sample], v->timescale);
    }

    /* sync samples */
    if (ti->stss.len >= 8)
        {
        u32 ne = rd32(ti->stss.p + 4), e;
        if (ne > (ti->stss.len - 8) / 4) ne = (ti->stss.len - 8) / 4;
        for (e = 0; e < ne; e++)
            {
            u32 s1 = rd32(ti->stss.p + 8 + 4 * e);
            if (s1 >= 1 && s1 <= n) v->key[s1 - 1] = 1;
            }
        v->key[0] = 1;
        }
    else for (i = 0; i < n; i++) v->key[i] = 1;

    for (i = 0; i < n; i++)
        {
        if ((int)v->size[i] > v->maxSize) v->maxSize = (int)v->size[i];
        v->totalBytes += v->size[i];
        }
    v->hasCtts = ti->ctts.len >= 8 && rd32(ti->ctts.p + 4) > 0;
    if (!v->durationMs && n) v->durationMs = v->timeMs[n - 1];
    return MP4_OK;
    }

static int parse_moov(Mp4File* f, Span moov)
    {
    u32 pos = 0;
    while (pos + 8 <= moov.len)
        {
        u32 size = rd32(moov.p + pos);
        u32 t = rd32(moov.p + pos + 4);
        u32 hdr = 8;
        if (size == 1)
            {
            if (pos + 16 > moov.len || rd32(moov.p + pos + 8)) break;
            size = rd32(moov.p + pos + 12);
            hdr = 16;
            }
        else if (size == 0) size = moov.len - pos;
        if (size < hdr || pos + size > moov.len) break;
        if (t == MP4_FOURCC('m','v','e','x')) { seterr(f, "fragmented MP4 is not supported"); return MP4_ERR_FRAGMENTED; }
        if (t == MP4_FOURCC('m','v','h','d') && size >= hdr + 20)
            {
            const u8* p = moov.p + pos + hdr;
            if (p[0] == 0) f->durationMs = to_ms(rd32(p + 16), rd32(p + 12));
            else if (size >= hdr + 32 && !rd32(p + 24)) f->durationMs = to_ms(rd32(p + 28), rd32(p + 20));
            if (!f->mvhd)
                {
                f->mvhd = (u8*)mp_alloc(&f->al, (int)size);
                if (f->mvhd) { mp_memcpy(f->mvhd, moov.p + pos, (int)size); f->mvhdLen = size; }
                }
            }
        if (t == MP4_FOURCC('t','r','a','k'))
            {
            TrakInfo ti;
            Span trak;
            trak.p = moov.p + pos + hdr;
            trak.len = size - hdr;
            parse_trak(trak, &ti);
            if (ti.handler == MP4_FOURCC('v','i','d','e') && !f->hasVideo)
                {
                int r = build_video(f, &ti);
                if (r == MP4_ERR_NOMEM) { seterr(f, "out of memory"); return r; }
                if (r == MP4_OK) f->hasVideo = 1;
                }
            else if (ti.handler == MP4_FOURCC('s','o','u','n') && !f->audio.present)
                {
                parse_audio(f, &ti);
                if (f->audio.present && !f->audioTrak)
                    {
                    f->audioTrak = (u8*)mp_alloc(&f->al, (int)size);
                    if (f->audioTrak) { mp_memcpy(f->audioTrak, moov.p + pos, (int)size); f->audioTrakLen = size; }
                    }
                }
            }
        pos += size;
        }
    return MP4_OK;
    }

int mp4_open(Mp4File** aFile, const MpIo* io, const MpAlloc* al)
    {
    Mp4File* f;
    u32 pos = 0;
    u8 hdr[16];
    int r = MP4_ERR_FORMAT, sawFtyp = 0;
    *aFile = 0;
    f = (Mp4File*)mp_alloc(al, sizeof(Mp4File));
    if (!f) return MP4_ERR_NOMEM;
    f->al = *al;
    seterr(f, "not an MP4/3GP file");
    while (pos + 8 <= io->size)
        {
        u32 size, type, h = 8;
        if (io->read(io->opaque, pos, hdr, 16) < 8) { r = MP4_ERR_IO; seterr(f, "read error"); break; }
        size = rd32(hdr);
        type = rd32(hdr + 4);
        if (size == 1)
            {
            if (rd32(hdr + 8)) { seterr(f, "file larger than 4 GB"); break; }
            size = rd32(hdr + 12);
            h = 16;
            }
        else if (size == 0) size = io->size - pos;
        if (size < h) break;
        if (pos == 0 && type != MP4_FOURCC('f','t','y','p') && type != MP4_FOURCC('m','o','o','v') &&
            type != MP4_FOURCC('m','d','a','t') && type != MP4_FOURCC('w','i','d','e') &&
            type != MP4_FOURCC('f','r','e','e') && type != MP4_FOURCC('s','k','i','p'))
            break;
        if (type == MP4_FOURCC('f','t','y','p')) { sawFtyp = 1; if (size >= 12) f->brand = rd32(hdr + 8); }
        if (type == MP4_FOURCC('m','d','a','t')) f->mdatFirst = 1;
        if (type == MP4_FOURCC('m','o','o','v'))
            {
            Span moov;
            u8* buf;
            u32 len = size - h;
            f->moovPos = pos;
            if (len > MAX_MOOV) { seterr(f, "moov too large"); break; }
            buf = (u8*)mp_alloc(al, len + 8);
            if (!buf) { r = MP4_ERR_NOMEM; seterr(f, "out of memory"); break; }
            if (io->read(io->opaque, pos + h, buf, (int)len) != (int)len)
                {
                mp_free(al, buf);
                r = MP4_ERR_IO;
                seterr(f, "read error");
                break;
                }
            moov.p = buf;
            moov.len = len;
            r = parse_moov(f, moov);
            mp_free(al, buf);
            if (r == MP4_OK && !f->hasVideo && !f->audio.present) { r = MP4_ERR_FORMAT; seterr(f, "no audio or video track"); }
            break;
            }
        if (pos + size < pos) break;            /* overflow */
        pos += size;
        }
    (void)sawFtyp;
    if (f->hasVideo && f->video.durationMs > f->durationMs) f->durationMs = f->video.durationMs;
    if (f->audio.durationMs > f->durationMs) f->durationMs = f->audio.durationMs;
    *aFile = f;
    if (r == MP4_OK) f->err[0] = 0;
    return r;
    }

void mp4_close(Mp4File* f)
    {
    MpAlloc al;
    if (!f) return;
    al = f->al;
    mp_free(&al, f->video.dsi);
    mp_free(&al, f->video.offset);
    mp_free(&al, f->video.size);
    mp_free(&al, f->video.timeMs);
    mp_free(&al, f->video.key);
    mp_free(&al, f->audioTrak);
    mp_free(&al, f->mvhd);
    mp_free(&al, f);
    }

int mp4_sample_at(const Mp4VideoTrack* t, u32 ms)
    {
    int lo = 0, hi = t->count - 1, best = 0;
    /* times are monotonic for streams without B-frames; binary search on them */
    while (lo <= hi)
        {
        int mid = (lo + hi) >> 1;
        if (t->timeMs[mid] <= ms) { best = mid; lo = mid + 1; }
        else hi = mid - 1;
        }
    return best;
    }

int mp4_key_before(const Mp4VideoTrack* t, int idx)
    {
    if (idx >= t->count) idx = t->count - 1;
    while (idx > 0 && !t->key[idx]) idx--;
    return idx < 0 ? 0 : idx;
    }

int mp4_key_after(const Mp4VideoTrack* t, int idx)
    {
    int i;
    for (i = idx + 1; i < t->count; i++) if (t->key[i]) return i;
    return -1;
    }

const char* mp4_codec_name(int codec)
    {
    switch (codec)
        {
        case MP4_VCODEC_MPEG4: return "MPEG-4 Part 2";
        case MP4_VCODEC_H263: return "H.263";
        case MP4_VCODEC_H264: return "H.264/AVC";
        case MP4_VCODEC_OTHER: return "unknown";
        default: return "none";
        }
    }

/* ------------------------------------------------------------------ audio-only copy */

struct Mp4AudioCopy
    {
    MpAlloc al;
    u8* head;                   /* ftyp + moov + mdat header, written first */
    u32 headLen;
    int headDone;
    u32* rOff;                  /* source ranges (adjacent chunks merged) */
    u32* rLen;
    int ranges;
    int cur;                    /* current range and bytes of it already copied */
    u32 curPos;
    u32 total, done;
    int is3gp;
    };

static void wr32(u8* p, u32 v) { p[0] = (u8)(v >> 24); p[1] = (u8)(v >> 16); p[2] = (u8)(v >> 8); p[3] = (u8)v; }

/* finds a box inside a box given with its own header */
static int find_child(const u8* box, u32 boxLen, u32 type, Span* out)
    {
    Span s;
    u32 hdr = 8;
    if (boxLen < 8) return 0;
    if (rd32(box) == 1) hdr = 16;
    if (boxLen < hdr) return 0;
    s.p = box + hdr;
    s.len = boxLen - hdr;
    return find_box(s, type, out);
    }

int mp4_audio_copy_begin(Mp4AudioCopy** aCopy, const Mp4File* f, const MpAlloc* al)
    {
    Mp4AudioCopy* c;
    Span mdia, minf, stbl, stsz, stz2, stsc, stco, co64;
    u8* trak;
    u32 nSamples, fixed = 0, nchunks, nstsc, e, sample = 0, outPos, base, moovLen, ftypLen = 28;
    int field = 0, is64 = 0;
    *aCopy = 0;
    if (!f->audio.present || !f->audioTrak || !f->mvhd) return MP4_ERR_FORMAT;
    c = (Mp4AudioCopy*)mp_alloc(al, sizeof(Mp4AudioCopy));
    if (!c) return MP4_ERR_NOMEM;
    c->al = *al;
    c->is3gp = f->audio.fourcc == MP4_FOURCC('s','a','m','r') || f->audio.fourcc == MP4_FOURCC('s','a','w','b');
    moovLen = 8 + f->mvhdLen + f->audioTrakLen;
    c->headLen = ftypLen + moovLen + 8;
    c->head = (u8*)mp_alloc(al, (int)c->headLen);
    if (!c->head) { mp4_audio_copy_end(c); return MP4_ERR_NOMEM; }

    /* ftyp like the files the phone plays: isom/iso2/mp41, or 3gp4 for AMR */
    wr32(c->head, ftypLen);
    wr32(c->head + 4, MP4_FOURCC('f','t','y','p'));
    if (c->is3gp)
        {
        wr32(c->head + 8, MP4_FOURCC('3','g','p','4')); wr32(c->head + 12, 0x200);
        wr32(c->head + 16, MP4_FOURCC('3','g','p','4')); wr32(c->head + 20, MP4_FOURCC('i','s','o','m'));
        wr32(c->head + 24, MP4_FOURCC('i','s','o','2'));
        }
    else
        {
        wr32(c->head + 8, MP4_FOURCC('i','s','o','m')); wr32(c->head + 12, 0x200);
        wr32(c->head + 16, MP4_FOURCC('i','s','o','m')); wr32(c->head + 20, MP4_FOURCC('i','s','o','2'));
        wr32(c->head + 24, MP4_FOURCC('m','p','4','1'));
        }
    wr32(c->head + ftypLen, moovLen);
    wr32(c->head + ftypLen + 4, MP4_FOURCC('m','o','o','v'));
    mp_memcpy(c->head + ftypLen + 8, f->mvhd, (int)f->mvhdLen);
    trak = c->head + ftypLen + 8 + f->mvhdLen;
    mp_memcpy(trak, f->audioTrak, (int)f->audioTrakLen);

    /* the sample tables inside our copy of the trak (stco gets patched there) */
    if (!find_child(trak, f->audioTrakLen, MP4_FOURCC('m','d','i','a'), &mdia) ||
        !find_box(mdia, MP4_FOURCC('m','i','n','f'), &minf) ||
        !find_box(minf, MP4_FOURCC('s','t','b','l'), &stbl) ||
        !find_box(stbl, MP4_FOURCC('s','t','s','c'), &stsc) || stsc.len < 8)
        { mp4_audio_copy_end(c); return MP4_ERR_FORMAT; }
    /* AAC whose sample entry says 2 channels while the AudioSpecificConfig says mono (MP4Box
       writes 2 always): give the phone's audio player the real channel count */
    {
    Span stsd;
    if (f->audio.fourcc == MP4_FOURCC('m','p','4','a') && f->audio.ascLen >= 2 &&
        find_box(stbl, MP4_FOURCC('s','t','s','d'), &stsd) && stsd.len >= 8 + 36 &&
        rd32(stsd.p + 12) == MP4_FOURCC('m','p','4','a'))
        {
        u8* ch = (u8*)stsd.p + 8 + 24;
        int cfg = (f->audio.asc[1] >> 3) & 15;
        int aot = f->audio.asc[0] >> 3, freq = ((f->audio.asc[0] & 7) << 1) | (f->audio.asc[1] >> 7);
        if (aot != 31 && freq != 15 && (cfg == 1 || cfg == 2) && (int)rd16(ch) != cfg) { ch[0] = 0; ch[1] = (u8)cfg; }
        }
    }
    if (find_box(stbl, MP4_FOURCC('s','t','s','z'), &stsz) && stsz.len >= 12)
        {
        fixed = rd32(stsz.p + 4);
        nSamples = rd32(stsz.p + 8);
        if (!fixed && nSamples > (stsz.len - 12) / 4) { mp4_audio_copy_end(c); return MP4_ERR_FORMAT; }
        }
    else if (find_box(stbl, MP4_FOURCC('s','t','z','2'), &stz2) && stz2.len >= 12)
        {
        field = stz2.p[7];
        nSamples = rd32(stz2.p + 8);
        if ((field != 4 && field != 8 && field != 16) || nSamples > ((stz2.len - 12) * 8) / (u32)field)
            { mp4_audio_copy_end(c); return MP4_ERR_FORMAT; }
        }
    else { mp4_audio_copy_end(c); return MP4_ERR_FORMAT; }
    if (find_box(stbl, MP4_FOURCC('s','t','c','o'), &stco) && stco.len >= 8)
        {
        nchunks = rd32(stco.p + 4);
        if (nchunks > (stco.len - 8) / 4) { mp4_audio_copy_end(c); return MP4_ERR_FORMAT; }
        }
    else if (find_box(stbl, MP4_FOURCC('c','o','6','4'), &co64) && co64.len >= 8)
        {
        is64 = 1;
        stco = co64;
        nchunks = rd32(co64.p + 4);
        if (nchunks > (co64.len - 8) / 8) { mp4_audio_copy_end(c); return MP4_ERR_FORMAT; }
        }
    else { mp4_audio_copy_end(c); return MP4_ERR_FORMAT; }
    nstsc = rd32(stsc.p + 4);
    if (nstsc == 0 || nstsc > (stsc.len - 8) / 12 || nchunks == 0) { mp4_audio_copy_end(c); return MP4_ERR_FORMAT; }

    c->rOff = (u32*)mp_alloc(al, 4 * (int)nchunks);
    c->rLen = (u32*)mp_alloc(al, 4 * (int)nchunks);
    if (!c->rOff || !c->rLen) { mp4_audio_copy_end(c); return MP4_ERR_NOMEM; }

    base = c->headLen;
    outPos = base;
    {
    u32 chunk = 1;              /* 1-based, every chunk gets a new offset (unused ones too) */
    for (e = 0; e < nstsc; e++)
        {
        u32 first = rd32(stsc.p + 8 + 12 * e);
        u32 per = rd32(stsc.p + 8 + 12 * e + 4);
        u32 lastChunk = (e + 1 < nstsc) ? rd32(stsc.p + 8 + 12 * (e + 1)) : nchunks + 1;
        if (first < chunk) first = chunk;
        for (; chunk < first && chunk <= nchunks; chunk++)     /* chunks not covered: empty */
            {
            if (is64) { wr32((u8*)stco.p + 8 + 8 * (chunk - 1), 0); wr32((u8*)stco.p + 12 + 8 * (chunk - 1), outPos); }
            else wr32((u8*)stco.p + 8 + 4 * (chunk - 1), outPos);
            }
        for (; chunk < lastChunk && chunk <= nchunks; chunk++)
            {
            u32 src, len = 0, k;
            if (is64)
                {
                if (rd32(stco.p + 8 + 8 * (chunk - 1)) != 0) { mp4_audio_copy_end(c); return MP4_ERR_FORMAT; }
                src = rd32(stco.p + 12 + 8 * (chunk - 1));
                }
            else src = rd32(stco.p + 8 + 4 * (chunk - 1));
            for (k = 0; k < per && sample < nSamples; k++, sample++)
                {
                u32 sz;
                if (fixed) sz = fixed;
                else if (field == 0) sz = rd32(stsz.p + 12 + 4 * sample);
                else if (field == 16) sz = rd16(stz2.p + 12 + 2 * sample);
                else if (field == 8) sz = stz2.p[12 + sample];
                else sz = (sample & 1) ? (stz2.p[12 + sample / 2] & 15) : (stz2.p[12 + sample / 2] >> 4);
                if (len + sz < len) { mp4_audio_copy_end(c); return MP4_ERR_FORMAT; }
                len += sz;
                }
            if (is64) { wr32((u8*)stco.p + 8 + 8 * (chunk - 1), 0); wr32((u8*)stco.p + 12 + 8 * (chunk - 1), outPos); }
            else wr32((u8*)stco.p + 8 + 4 * (chunk - 1), outPos);
            if (len)
                {
                if (outPos + len < outPos || src + len < src) { mp4_audio_copy_end(c); return MP4_ERR_FORMAT; }
                if (c->ranges > 0 && c->rOff[c->ranges - 1] + c->rLen[c->ranges - 1] == src)
                    c->rLen[c->ranges - 1] += len;
                else
                    {
                    c->rOff[c->ranges] = src;
                    c->rLen[c->ranges] = len;
                    c->ranges++;
                    }
                outPos += len;
                }
            }
        }
    for (; chunk <= nchunks; chunk++)
        {
        if (is64) { wr32((u8*)stco.p + 8 + 8 * (chunk - 1), 0); wr32((u8*)stco.p + 12 + 8 * (chunk - 1), outPos); }
        else wr32((u8*)stco.p + 8 + 4 * (chunk - 1), outPos);
        }
    }
    if (outPos == base) { mp4_audio_copy_end(c); return MP4_ERR_FORMAT; }
    wr32(c->head + ftypLen + moovLen, outPos - base + 8);
    wr32(c->head + ftypLen + moovLen + 4, MP4_FOURCC('m','d','a','t'));
    c->total = outPos;
    *aCopy = c;
    return MP4_OK;
    }

/* source data closer than this to the previous range is read in the same read */
#define AUDIO_COPY_GAP 16384

int mp4_audio_copy_step(Mp4AudioCopy* c, const MpIo* io, u8* buf, int bufLen, Mp4WriteFn write, void* wop)
    {
    u32 start, end, pos;
    int r, n, i;
    u32 outLen = 0;
    if (!c->headDone)
        {
        if (write(wop, c->head, (int)c->headLen)) return -1;
        c->headDone = 1;
        c->done = c->headLen;
        return c->cur >= c->ranges ? 1 : 0;
        }
    if (c->cur >= c->ranges) return 1;
    /* the read window: from the current position over the following ranges that fit */
    start = c->rOff[c->cur] + c->curPos;
    end = start + (c->rLen[c->cur] - c->curPos);
    if (end - start > (u32)bufLen) end = start + (u32)bufLen;
    for (i = c->cur + 1; i < c->ranges && end == c->rOff[i - 1] + c->rLen[i - 1]; i++)
        {
        u32 o = c->rOff[i];
        if (o < end || o - end > AUDIO_COPY_GAP || o - start > (u32)bufLen || c->rLen[i] > (u32)bufLen - (o - start)) break;
        end = o + c->rLen[i];
        }
    if (end > io->size) return -2;                      /* the file is shorter than its tables */
    r = io->read(io->opaque, start, buf, (int)(end - start));
    if (r != (int)(end - start)) return -1;
    /* pack the audio parts of the window to the front of buf */
    pos = start;
    while (c->cur < c->ranges)
        {
        u32 from = c->rOff[c->cur] + c->curPos;
        u32 left = c->rLen[c->cur] - c->curPos;
        u8* d;
        const u8* s;
        if (from < pos || from >= end) break;     /* overlapping ranges (damaged file): next window */
        if (left > end - from) left = end - from;
        d = buf + outLen;
        s = buf + (from - start);
        if (d != s) for (n = 0; n < (int)left; n++) d[n] = s[n];      /* d < s: forward copy is safe */
        outLen += left;
        c->curPos += left;
        if (c->curPos == c->rLen[c->cur]) { c->cur++; c->curPos = 0; }
        else break;
        pos = from + left;
        }
    if (outLen && write(wop, buf, (int)outLen)) return -1;
    c->done += outLen;
    return c->cur >= c->ranges ? 1 : 0;
    }

u32 mp4_audio_copy_total(const Mp4AudioCopy* c) { return c->total; }
u32 mp4_audio_copy_done(const Mp4AudioCopy* c) { return c->done; }
int mp4_audio_copy_is_3gp(const Mp4AudioCopy* c) { return c->is3gp; }

void mp4_audio_copy_end(Mp4AudioCopy* c)
    {
    MpAlloc al;
    if (!c) return;
    al = c->al;
    mp_free(&al, c->head);
    mp_free(&al, c->rOff);
    mp_free(&al, c->rLen);
    mp_free(&al, c);
    }
