// PC check of the "Analyze a file" data (demuxer + decoder statistics), same fields as the phone.
// usage: analyze file.mp4
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "mp4demux.h"
#include "viddec.h"

static void* a_alloc(void*, int n) { void* p = malloc(n); if (p) memset(p, 0, n); return p; }
static void a_free(void*, void* p) { free(p); }
static int f_read(void* o, u32 off, void* buf, int len)
    {
    FILE* f = (FILE*)o;
    fseek(f, off, SEEK_SET);
    return (int)fread(buf, 1, len, f);
    }
static void fourcc(u32 c) { for (int i = 3; i >= 0; i--) putchar((int)((c >> (8 * i)) & 0xFF)); }

int main(int argc, char** argv)
    {
    if (argc < 2) return 1;
    FILE* in = fopen(argv[1], "rb");
    if (!in) return 1;
    fseek(in, 0, SEEK_END);
    MpIo io; io.read = f_read; io.opaque = in; io.size = (u32)ftell(in);
    MpAlloc al; al.alloc = a_alloc; al.free = a_free; al.opaque = 0;
    Mp4File* f = 0;
    int r = mp4_open(&f, &io, &al);
    printf("size %u kB, r %d, brand ", io.size / 1024, r); fourcc(f->brand);
    printf(", %s, duration %u ms\n", f->mdatFirst ? "index at end" : "index at start", f->durationMs);
    if (r) return 1;
    const Mp4VideoTrack* t = &f->video;
    if (f->hasVideo)
        {
        int keys = 0, maxGap = 0, last = -1;
        for (int i = 0; i < t->count; i++) if (t->key[i]) { if (last >= 0 && i - last > maxGap) maxGap = i - last; last = i; keys++; }
        if (t->count - last > maxGap) maxGap = t->count - last;
        printf("video %s (", mp4_codec_name(t->codec)); fourcc(t->fourcc);
        printf(") %dx%d %d frames %.2f fps %.0f kbit/s avg %u max %d keys %d gap %d ctts %d\n", t->width, t->height, t->count,
               t->count * 1000.0 / t->durationMs, t->totalBytes * 8.0 / t->durationMs, t->totalBytes / t->count, t->maxSize, keys, maxGap, t->hasCtts);
        }
    if (f->audio.present)
        {
        printf("audio "); fourcc(f->audio.fourcc);
        printf(" obj 0x%02x %d Hz %dch %.0f kbit/s ASC", f->audio.objectType, f->audio.sampleRate, f->audio.channels,
               f->audio.totalBytes * 8.0 / (f->audio.durationMs ? f->audio.durationMs : f->durationMs));
        for (int i = 0; i < f->audio.ascLen; i++) printf(" %02x", f->audio.asc[i]);
        printf("\n");
        }
    if (!f->hasVideo) return 0;
    VidDec* d = 0;
    r = viddec_open(&d, &al, t);
    if (t->codec == MP4_VCODEC_H264 && t->dsiLen >= 7)
        printf("avcC profile %d flags %02x level %d nal %d\n", t->dsi[1], t->dsi[2], t->dsi[3], (t->dsi[4] & 3) + 1);
    if (r) { printf("decoder: %s\n", d ? viddec_error_text(d) : "?"); return 0; }
    int n = t->count < 100 ? t->count : 100, cnt[8] = {0};
    u8* buf = (u8*)malloc(t->maxSize + MP_INPUT_PADDING);
    for (int i = 0; i < n; i++)
        {
        M4vPicture pic;
        f_read(in, t->offset[i], buf, t->size[i]);
        memset(buf + t->size[i], 0, MP_INPUT_PADDING);
        int x = viddec_decode(d, buf, t->size[i], &pic);
        cnt[x >= 0 && x <= 4 ? x : 5]++;
        }
    printf("results I %d P %d skip %d rep %d none %d err %d\n", cnt[0], cnt[1], cnt[2], cnt[3], cnt[4], cnt[5]);
    H264Params hp; H264Stats hs; M4vParams mp;
    if (viddec_h264_info(d, &hp, &hs))
        {
        printf("SPS valid %d profile %d level %d %dx%d MBs crop %d L%d R%d T%d B%d refs %d poc %d fnbits %d vui %d timing %d %d/%d fixed %d restr %d reorder %d dpb %d\n",
               hp.valid, hp.profile, hp.level, hp.mbW, hp.mbH, hp.crop, hp.cropL, hp.cropR, hp.cropT, hp.cropB, hp.refs, hp.pocType,
               hp.frameNumBits, hp.vui, hp.timing, hp.numUnitsInTick, hp.timeScale, hp.fixedRate, hp.restrict_, hp.reorder, hp.maxDecBuf);
        printf("PPS valid %d cabac %d qp %d cqp %d refidx %d dbctrl %d ci %d sg %d red %d\n", hp.ppsValid, hp.cabac, hp.initQp,
               hp.chromaQpOff, hp.refIdx, hp.deblockCtrl, hp.constrIntra, hp.sliceGroups, hp.redundant);
        printf("slices %d (max %d) I %d P %d B %d other %d idr %d decoded %d avgQP %.1f dboff %d nal %d sei %d aud %d ps %d other %d\n",
               hs.slices, hs.maxSlicesPerSample, hs.iSlices, hs.pSlices, hs.bSlices, hs.otherSlices, hs.idrSlices, hs.decoded,
               hs.decoded ? (double)hs.qpSum / hs.decoded : 0.0, hs.deblockOff, hs.nals, hs.sei, hs.aud, hs.paramSets, hs.other);
        }
    if (viddec_m4v_info(d, &mp))
        printf("VOL %d short %d verid %d quant %d resync %d tbits %d\n", mp.haveVol, mp.shortHeader, mp.verid, mp.quantType, mp.resyncMarker, mp.timeIncBits);
    return 0;
    }
