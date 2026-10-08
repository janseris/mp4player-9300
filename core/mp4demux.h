/*
 * Minimal ISO base media file (MP4 / 3GP / 3G2 / MOV) demuxer: reads the moov box and builds
 * the sample table of the first video track. Audio tracks are only described (the phone's
 * own MMF audio controller plays the audio from the same file). Because that controller
 * refuses files with H.264 video, the audio track can also be copied into a separate
 * audio-only MP4/3GP file (mp4_audio_copy_*).
 */
#ifndef MP4DEMUX_H
#define MP4DEMUX_H
#include "mpcommon.h"

#define MP4_FOURCC(a,b,c,d) (((u32)(a) << 24) | ((u32)(b) << 16) | ((u32)(c) << 8) | (u32)(d))

enum
    {
    MP4_VCODEC_NONE = 0,
    MP4_VCODEC_MPEG4 = 1,       /* MPEG-4 Part 2 (mp4v, object type 0x20) */
    MP4_VCODEC_H263 = 2,        /* s263 / h263 */
    MP4_VCODEC_H264 = 3,        /* avc1 (not decodable here) */
    MP4_VCODEC_OTHER = 4
    };

struct Mp4VideoTrack
    {
    int codec;                  /* MP4_VCODEC_* */
    u32 fourcc;
    int width, height;
    u32 timescale;
    u32 durationMs;
    u8* dsi;                    /* decoder specific info (VOL headers), may be NULL */
    int dsiLen;
    int count;                  /* number of samples */
    u32* offset;
    u32* size;
    u32* timeMs;                /* presentation time of each sample */
    u8* key;                    /* 1 = sync sample */
    int maxSize;                /* largest sample */
    };

struct Mp4AudioInfo
    {
    int present;
    u32 fourcc;                 /* mp4a, samr, sawb, ... */
    int objectType;             /* 0x40 = AAC (from esds) */
    int channels;
    int sampleRate;
    u32 durationMs;
    };

struct Mp4File
    {
    MpAlloc al;
    int hasVideo;
    Mp4VideoTrack video;
    Mp4AudioInfo audio;
    u32 durationMs;
    u8* audioTrak;              /* copy of the first sound 'trak' box (with its header) */
    u32 audioTrakLen;
    u8* mvhd;                   /* copy of the 'mvhd' box (with its header) */
    u32 mvhdLen;
    char err[48];
    };

enum
    {
    MP4_OK = 0,
    MP4_ERR_IO = -1,
    MP4_ERR_FORMAT = -2,        /* not an MP4 file / damaged */
    MP4_ERR_NOMEM = -3,
    MP4_ERR_FRAGMENTED = -4
    };

int  mp4_open(Mp4File** aFile, const MpIo* io, const MpAlloc* al);
void mp4_close(Mp4File* f);
/* index of the last sample with time <= ms (0 if none) */
int  mp4_sample_at(const Mp4VideoTrack* t, u32 ms);
/* nearest sync sample at or before idx */
int  mp4_key_before(const Mp4VideoTrack* t, int idx);
/* nearest sync sample after idx, or -1 */
int  mp4_key_after(const Mp4VideoTrack* t, int idx);
const char* mp4_codec_name(int codec);

/*
 * Audio-only copy: ftyp + moov (mvhd + the original sound trak with new chunk offsets) +
 * mdat with the audio chunks packed together. Same audio, nothing else the phone's
 * MP4/3GP audio controller could object to.
 */
struct Mp4AudioCopy;
typedef int (*Mp4WriteFn)(void* opaque, const u8* data, int len);   /* 0 = ok */

/* MP4_OK, MP4_ERR_FORMAT (no usable audio track) or MP4_ERR_NOMEM */
int  mp4_audio_copy_begin(Mp4AudioCopy** c, const Mp4File* f, const MpAlloc* al);
/* copies the next part (at most about bufLen bytes read, buf is scratch space of bufLen
   bytes); returns 1 when the copy is complete, 0 when there is more to do, <0 on error */
int  mp4_audio_copy_step(Mp4AudioCopy* c, const MpIo* io, u8* buf, int bufLen, Mp4WriteFn write, void* wop);
u32  mp4_audio_copy_total(const Mp4AudioCopy* c);      /* bytes of the whole output file */
u32  mp4_audio_copy_done(const Mp4AudioCopy* c);       /* bytes written so far */
int  mp4_audio_copy_is_3gp(const Mp4AudioCopy* c);     /* AMR audio: use a .3gp name */
void mp4_audio_copy_end(Mp4AudioCopy* c);

#endif
