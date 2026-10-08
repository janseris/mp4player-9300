// corrupts samples randomly and decodes; run under ASAN/UBSAN
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../core/mp4demux.h"
#include "../core/m4vdec.h"
#include "../core/viddec.h"
static void* a_alloc(void*, int n) { return malloc(n); }
static void a_free(void*, void* p) { free(p); }
struct Mem { unsigned char* p; u32 n; };
static int m_read(void* o, u32 off, void* buf, int len) { Mem* m=(Mem*)o; if (off>=m->n) return 0; if (off+len>m->n) len=m->n-off; memcpy(buf,m->p+off,len); return len; }
int main(int argc, char** argv)
    {
    FILE* in = fopen(argv[1], "rb"); fseek(in,0,SEEK_END); u32 n=ftell(in); fseek(in,0,SEEK_SET);
    Mem m; m.p=(unsigned char*)malloc(n); m.n=n; fread(m.p,1,n,in); fclose(in);
    int iters = atoi(argv[2]); srand(atoi(argv[3]));
    MpAlloc al; al.alloc=a_alloc; al.free=a_free; al.opaque=0;
    long total=0;
    for (int it=0; it<iters; it++)
        {
        Mem c; c.n=n; c.p=(unsigned char*)malloc(n); memcpy(c.p,m.p,n);
        int mode = it % 3;
        int flips = 1 + rand()%20;
        for (int k=0;k<flips;k++) {
            u32 pos = rand()%n;
            if (mode==0 && it%7==0) pos = rand()% (n<4096?n:4096);  // container damage
            c.p[pos] ^= (1<<(rand()%8));
            if (mode==2) c.p[pos] = rand();
        }
        MpIo io; io.read=m_read; io.opaque=&c; io.size=n;
        Mp4File* f=0;
        if (mp4_open(&f,&io,&al)==0 && f->hasVideo)
            {
            VidDec* d=0;
            int ro = viddec_open(&d,&al,&f->video);
            if (ro != 0) { viddec_close(d); mp4_close(f); free(c.p); continue; }
            unsigned char* buf=(unsigned char*)malloc(f->video.maxSize+MP_INPUT_PADDING);
            for (int i=0;i<f->video.count;i++)
                {
                u32 sz=f->video.size[i], off=f->video.offset[i];
                if (off>=n || sz>n-off) continue;
                memcpy(buf,c.p+off,sz); memset(buf+sz,0,MP_INPUT_PADDING);
                M4vPicture pic; int r=viddec_decode(d,buf,sz,&pic);
                if (r==M4V_FRAME_I||r==M4V_FRAME_P||r==M4V_FRAME_REPEAT) { total += pic.y[0] + pic.y[(pic.height-1)*pic.ystride+pic.width-1]; }
                }
            free(buf); viddec_close(d);
            }
        if (f) mp4_close(f);
        free(c.p);
        }
    printf("ok %ld\n", total);
    return 0;
    }
