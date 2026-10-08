/*
 * MP4 Player 9300: H.264 decoder for MP4 'avc1' tracks, on top of h264bsd (Baseline profile:
 * I/P slices, CAVLC, no interlace). C interface, usable from C++.
 */
#ifndef H264DEC_H
#define H264DEC_H

#ifdef __cplusplus
extern "C" {
#endif

typedef struct H264Dec H264Dec;

/* same layout as M4vPicture (core/m4vdec.h) */
typedef struct
    {
    const unsigned char* y;
    const unsigned char* u;
    const unsigned char* v;
    int ystride;
    int cstride;
    int width;      /* display size (cropping applied) */
    int height;
    } H264Pic;

/* return values: >= 0 as in m4vdec.h (0 = key frame, 1 = other frame, 4 = no picture) */
#define H264D_FRAME_KEY     0
#define H264D_FRAME         1
#define H264D_NOFRAME       4
#define H264D_ERR_GENERIC   (-1)
#define H264D_ERR_NOMEM     (-2)
#define H264D_ERR_UNSUPPORTED (-3)

int  h264d_open(H264Dec** aDec);
void h264d_close(H264Dec* d);

/* avcC box payload (AVCDecoderConfigurationRecord): decodes its SPS/PPS. The buffer is modified.
   Returns the NAL length size (1, 2 or 4) or < 0. */
int  h264d_config(H264Dec* d, unsigned char* avcc, int len);

/* one MP4 sample (length-prefixed NAL units). The buffer is modified. */
int  h264d_decode(H264Dec* d, unsigned char* buf, int len, H264Pic* pic);

/* 1: skip the deblocking (loop) filter: about a third faster, slightly blocky pictures that
   drift a little until the next key frame */
void h264d_set_fast(H264Dec* d, int fast);

/* after a seek: forget reference pictures */
void h264d_flush(H264Dec* d);

/* profile_idc from the SPS (66 Baseline, 77 Main, 100 High), 0 if unknown */
int  h264d_profile(H264Dec* d);
/* number of slices that failed to decode (for the info screen) */
int  h264d_errors(H264Dec* d);

#ifdef __cplusplus
}
#endif

#endif
