// Host test: writes the audio-only copy the phone player makes for H.264 files.
// usage: audiocopy in.mp4 out.mp4 [bufsize]
#include <stdio.h>
#include <stdlib.h>
#include "../core/mp4demux.h"

static void* a_alloc(void*, int n) { return malloc(n); }
static void a_free(void*, void* p) { free(p); }
static int f_read(void* o, u32 off, void* buf, int len)
    {
    FILE* f = (FILE*)o;
    if (fseek(f, off, SEEK_SET)) return -1;
    return (int)fread(buf, 1, len, f);
    }
static int f_write(void* o, const u8* p, int n) { return fwrite(p, 1, n, (FILE*)o) == (size_t)n ? 0 : -1; }

int main(int argc, char** argv)
    {
    if (argc < 3) { fprintf(stderr, "usage: audiocopy in.mp4 out.mp4 [bufsize]\n"); return 2; }
    int bufLen = argc > 3 ? atoi(argv[3]) : 65536;
    FILE* in = fopen(argv[1], "rb");
    if (!in) { perror("open"); return 1; }
    fseek(in, 0, SEEK_END);
    MpIo io; io.read = f_read; io.opaque = in; io.size = (u32)ftell(in);
    MpAlloc al; al.alloc = a_alloc; al.free = a_free; al.opaque = 0;
    Mp4File* mf = 0;
    int r = mp4_open(&mf, &io, &al);
    if (r) { fprintf(stderr, "mp4_open: %d\n", r); return 1; }
    Mp4AudioCopy* c = 0;
    r = mp4_audio_copy_begin(&c, mf, &al);
    if (r) { fprintf(stderr, "copy_begin: %d\n", r); return 1; }
    FILE* out = fopen(argv[2], "wb");
    u8* buf = (u8*)malloc(bufLen);
    int steps = 0;
    while ((r = mp4_audio_copy_step(c, &io, buf, bufLen, f_write, out)) == 0) steps++;
    if (r < 0) { fprintf(stderr, "step: %d\n", r); return 1; }
    fclose(out);
    fprintf(stderr, "%u bytes in %d steps, 3gp=%d\n", mp4_audio_copy_total(c), steps + 1, mp4_audio_copy_is_3gp(c));
    if (mp4_audio_copy_done(c) != mp4_audio_copy_total(c)) { fprintf(stderr, "size mismatch\n"); return 1; }
    mp4_audio_copy_end(c);
    mp4_close(mf);
    free(buf);
    fclose(in);
    return 0;
    }
