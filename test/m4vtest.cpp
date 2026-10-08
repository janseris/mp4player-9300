// Host test: demux an MP4/3GP, decode the video track, write raw I420 frames (display size).
// usage: m4vtest in.mp4 out.yuv
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "../core/mp4demux.h"
#include "../core/m4vdec.h"
#include "../core/viddec.h"

static void* a_alloc(void*, int n) { return malloc(n); }
static void a_free(void*, void* p) { free(p); }
static int f_read(void* o, u32 off, void* buf, int len)
    {
    FILE* f = (FILE*)o;
    if (fseek(f, off, SEEK_SET)) return -1;
    return (int)fread(buf, 1, len, f);
    }

int main(int argc, char** argv)
    {
    if (argc < 3) { fprintf(stderr, "usage: m4vtest in.mp4 out.yuv [repeatnoref]\n"); return 2; }
    FILE* in = fopen(argv[1], "rb");
    if (!in) { perror("open"); return 1; }
    fseek(in, 0, SEEK_END);
    MpIo io; io.read = f_read; io.opaque = in; io.size = (u32)ftell(in);
    MpAlloc al; al.alloc = a_alloc; al.free = a_free; al.opaque = 0;
    Mp4File* mf = 0;
    int r = mp4_open(&mf, &io, &al);
    if (r) { fprintf(stderr, "mp4_open: %d %s\n", r, mf ? mf->err : ""); return 1; }
    fprintf(stderr, "video=%d codec=%s %dx%d samples=%d dur=%u ms audio=%d (%.4s obj=0x%x %d Hz %d ch)\n",
            mf->hasVideo, mp4_codec_name(mf->video.codec), mf->video.width, mf->video.height,
            mf->video.count, mf->durationMs, mf->audio.present, (char*)&mf->audio.fourcc,
            mf->audio.objectType, mf->audio.sampleRate, mf->audio.channels);
    VidDec* d = 0;
    r = viddec_open(&d, &al, &mf->video);
    if (r) { fprintf(stderr, "open: %d %s\n", r, d ? viddec_error_text(d) : ""); if (r != M4V_ERR_UNSUPPORTED || !d) return 1; }
    if (getenv("FAST")) viddec_set_fast(d, atoi(getenv("FAST")));
    FILE* out = fopen(argv[2], "wb");
    unsigned char* buf = (unsigned char*)malloc(mf->video.maxSize + MP_INPUT_PADDING);
    int frames = 0, errs = 0;
    clock_t t0 = clock();
    M4vPicture last; memset(&last, 0, sizeof(last));
    int haveLast = 0;
    u32 hash = 2166136261u;
    int hashN = getenv("CHECKN") ? atoi(getenv("CHECKN")) : 150;   /* frames in the check value */
    for (int i = 0; i < mf->video.count; i++)
        {
        int sz = mf->video.size[i];
        f_read(in, mf->video.offset[i], buf, sz);
        memset(buf + sz, 0, MP_INPUT_PADDING);
        M4vPicture pic;
        r = viddec_decode(d, buf, sz, &pic);
        if (r < 0) { errs++; fprintf(stderr, "frame %d: err %d %s\n", i, r, viddec_error_text(d)); continue; }
        if (r == M4V_FRAME_SKIPPED || r == M4V_NOFRAME) continue;
        last = pic; haveLast = 1;
        if (i < hashN) hash = m4v_picture_hash(&pic, hash);
        for (int y = 0; y < pic.height; y++) fwrite(pic.y + y * pic.ystride, 1, pic.width, out);
        for (int y = 0; y < (pic.height + 1) / 2; y++) fwrite(pic.u + y * pic.cstride, 1, (pic.width + 1) / 2, out);
        for (int y = 0; y < (pic.height + 1) / 2; y++) fwrite(pic.v + y * pic.cstride, 1, (pic.width + 1) / 2, out);
        frames++;
        }
    double secs = (double)(clock() - t0) / CLOCKS_PER_SEC;
    fprintf(stderr, "decoded %d frames, %d errors, %.3f s (%.1f fps) info: %s\n", frames, errs, secs, frames / (secs > 0 ? secs : 1), viddec_stream_info(d));
    (void)haveLast;
    fprintf(stderr, "check (first %d samples): %08X\n", hashN, hash);
    fclose(out);
    viddec_close(d);
    mp4_close(mf);
    free(buf);
    return 0;
    }
