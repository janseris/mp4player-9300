/*
 * MP4 Player 9300: one interface for the video decoders (MPEG-4 Part 2 / H.263 and H.264).
 * Return values and the picture are those of m4vdec.h.
 *
 * The H.264 decoder (h264bsd) allocates with h264port_malloc: on the phone that is the current
 * thread heap, so the caller switches to its decoder heap around viddec_open/decode/close.
 */
#ifndef VIDDEC_H
#define VIDDEC_H
#include "mpcommon.h"
#include "m4vdec.h"
#include "mp4demux.h"

struct VidDec;

/* opens the decoder for the track's codec and applies its decoder specific info;
   returns 0, M4V_ERR_UNSUPPORTED (codec not decodable here, error text says why) or another error */
int  viddec_open(VidDec** aDec, const MpAlloc* aAlloc, const Mp4VideoTrack* aTrack);
void viddec_close(VidDec* d);
/* buf: one sample followed by MP_INPUT_PADDING zero bytes; may be modified */
int  viddec_decode(VidDec* d, u8* buf, int len, M4vPicture* pic);
void viddec_flush(VidDec* d);
const char* viddec_error_text(VidDec* d);
const char* viddec_stream_info(VidDec* d);
int  viddec_is_h264(VidDec* d);
/* H.264 only: 1 = skip the deblocking filter (faster) */
void viddec_set_fast(VidDec* d, int fast);
/* H.264 only: see h264d_set_spare; viddec_spare() = how many pictures may be decoded ahead + 1 */
void viddec_set_spare(VidDec* d, int count, int budget);
int  viddec_spare(VidDec* d);

#endif
