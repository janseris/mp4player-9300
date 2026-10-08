/* YUV 4:2:0 -> RGB conversion with nearest-neighbour scaling (BT.601, limited range). */
#ifndef YUV2RGB_H
#define YUV2RGB_H
#include "mpcommon.h"
#include "m4vdec.h"

enum
    {
    YUV_RGB565 = 0,     /* Symbian EColor64K */
    YUV_RGB444 = 1,     /* EColor4K: 0000RRRRGGGGBBBB */
    YUV_BGR888 = 2,     /* EColor16M: bytes B,G,R */
    YUV_XRGB8888 = 3    /* EColor16MU */
    };

struct YuvConv;

int  yuvconv_open(YuvConv** c, const MpAlloc* al);
void yuvconv_close(YuvConv* c);
/* source picture size, destination rectangle size and pixel format */
int  yuvconv_setup(YuvConv* c, int srcW, int srcH, int dstW, int dstH, int format);
/* same, scaling only the source window (cx, cy, cw, ch) - used to crop ("fill" zoom) */
int  yuvconv_setup_crop(YuvConv* c, int cx, int cy, int cw, int ch, int dstW, int dstH, int format);
/* converts; dst points at the top-left pixel of the destination rectangle */
void yuvconv_run(YuvConv* c, const M4vPicture* pic, u8* dst, int dstStride);

#endif
