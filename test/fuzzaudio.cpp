// damages the moov box randomly and runs the audio-only copy; run under ASAN/UBSAN
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../core/mp4demux.h"
static void* a_alloc(void*, int n) { return malloc(n); }
static void a_free(void*, void* p) { free(p); }
struct Mem { unsigned char* p; u32 n; };
static int m_read(void* o, u32 off, void* buf, int len) { Mem* m=(Mem*)o; if (off>=m->n) return 0; if (off+len>m->n) len=m->n-off; memcpy(buf,m->p+off,len); return len; }
static int w_null(void* o, const u8*, int n) { *(long*)o += n; return 0; }
static u32 be(const unsigned char* p) { return (p[0]<<24)|(p[1]<<16)|(p[2]<<8)|p[3]; }
int main(int argc, char** argv)
    {
    FILE* in = fopen(argv[1], "rb"); fseek(in,0,SEEK_END); u32 n=ftell(in); fseek(in,0,SEEK_SET);
    Mem m; m.p=(unsigned char*)malloc(n); m.n=n; if (fread(m.p,1,n,in)!=n) return 1; fclose(in);
    int iters = atoi(argv[2]); srand(atoi(argv[3]));
    u32 moov=0, moovLen=n, pos=0;
    while (pos+8<=n) { u32 s=be(m.p+pos); if (!memcmp(m.p+pos+4,"moov",4)) { moov=pos; moovLen=s; break; } if (s<8) break; pos+=s; }
    MpAlloc al; al.alloc=a_alloc; al.free=a_free; al.opaque=0;
    int ok=0, opened=0;
    for (int it=0; it<iters; it++)
        {
        Mem c; c.n=n; c.p=(unsigned char*)malloc(n); memcpy(c.p,m.p,n);
        int flips = 1 + rand()%8;
        for (int k=0;k<flips;k++) { u32 p = moov + rand()%moovLen; if (it&1) c.p[p] ^= (1<<(rand()%8)); else c.p[p]=rand(); }
        MpIo io; io.read=m_read; io.opaque=&c; io.size=n;
        Mp4File* f=0;
        if (mp4_open(&f,&io,&al)==0)
            {
            Mp4AudioCopy* cp=0;
            if (mp4_audio_copy_begin(&cp,f,&al)==0)
                {
                opened++;
                unsigned char buf[3000]; long written=0; int r;
                while ((r=mp4_audio_copy_step(cp,&io,buf,sizeof(buf),w_null,&written))==0) {}
                if (r==1 && (u32)written==mp4_audio_copy_total(cp)) ok++;
                mp4_audio_copy_end(cp);
                }
            }
        if (f) mp4_close(f);
        free(c.p);
        }
    printf("copies %d complete %d\n", opened, ok);
    free(m.p);
    return 0;
    }
