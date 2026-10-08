// decodes frame N and writes it through yuvconv (RGB565, scaled to WxH) as PPM
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../core/mp4demux.h"
#include "../core/m4vdec.h"
#include "../core/yuv2rgb.h"
static void* a_alloc(void*, int n) { return malloc(n); }
static void a_free(void*, void* p) { free(p); }
static int f_read(void* o, u32 off, void* buf, int len) { FILE* f=(FILE*)o; fseek(f,off,SEEK_SET); return (int)fread(buf,1,len,f); }
int main(int argc, char** argv)
    {
    FILE* in=fopen(argv[1],"rb"); fseek(in,0,SEEK_END);
    MpIo io; io.read=f_read; io.opaque=in; io.size=ftell(in);
    MpAlloc al; al.alloc=a_alloc; al.free=a_free; al.opaque=0;
    Mp4File* mf; mp4_open(&mf,&io,&al);
    M4vDec* d; m4v_open(&d,&al); if (mf->video.dsiLen) m4v_config(d,mf->video.dsi,mf->video.dsiLen);
    int N=atoi(argv[3]), W=atoi(argv[4]), H=atoi(argv[5]);
    unsigned char* buf=(unsigned char*)malloc(mf->video.maxSize+16);
    M4vPicture pic;
    for (int i=0;i<=N;i++){ f_read(in,mf->video.offset[i],buf,mf->video.size[i]); memset(buf+mf->video.size[i],0,16); m4v_decode(d,buf,mf->video.size[i],&pic); }
    YuvConv* c; yuvconv_open(&c,&al); if (argc > 9) yuvconv_setup_crop(c,atoi(argv[6]),atoi(argv[7]),atoi(argv[8]),atoi(argv[9]),W,H,YUV_RGB565); else yuvconv_setup(c,pic.width,pic.height,W,H,YUV_RGB565);
    unsigned short* rgb=(unsigned short*)malloc(W*H*2); yuvconv_run(c,&pic,(u8*)rgb,W*2);
    FILE* o=fopen(argv[2],"wb"); fprintf(o,"P6 %d %d 255\n",W,H);
    for(int k=0;k<W*H;k++){unsigned v=rgb[k]; unsigned char p[3]={(unsigned char)((v>>11)<<3),(unsigned char)(((v>>5)&63)<<2),(unsigned char)((v&31)<<3)}; fwrite(p,1,3,o);} fclose(o);
    return 0;
    }
