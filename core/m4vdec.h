/*
 * MPEG-4 Part 2 (Simple Profile) and H.263 baseline ("short header") video decoder.
 *
 * Supported: I-, P- and not-coded VOPs, H.263 and MPEG quantisation (incl. custom matrices),
 * AC/DC prediction, 1MV/4MV, unrestricted motion vectors, half-pel MC with rounding control,
 * video packets (resync markers), H.263 baseline pictures (sub-QCIF..16CIF) with GOB headers.
 * Not supported (the frame is skipped or an error returned): B-VOPs, GMC/sprites, quarter-pel,
 * interlace, data partitioning/RVLC, non-rectangular shapes, H.263+ optional modes.
 */
#ifndef M4VDEC_H
#define M4VDEC_H
#include "mpcommon.h"

struct M4vDec;

struct M4vPicture
    {
    const u8* y;
    const u8* u;
    const u8* v;
    int ystride;
    int cstride;
    int width;      /* display size */
    int height;
    };

enum
    {
    M4V_FRAME_I = 0,
    M4V_FRAME_P = 1,
    M4V_FRAME_SKIPPED = 2,      /* B-VOP or undecodable frame: nothing new to show */
    M4V_FRAME_REPEAT = 3,       /* not-coded VOP: show the previous picture again */
    M4V_NOFRAME = 4             /* only headers in this buffer */
    };

enum
    {
    M4V_ERR_GENERIC = -1,
    M4V_ERR_NOMEM = -2,
    M4V_ERR_UNSUPPORTED = -3,
    M4V_ERR_NOREF = -4,         /* P-frame without a reference (after a seek): skip to a key frame */
    M4V_ERR_BITSTREAM = -5
    };

int  m4v_open(M4vDec** aDec, const MpAlloc* aAlloc);
void m4v_close(M4vDec* d);

/* Decoder specific info from the container (VOS/VO/VOL headers). Optional. */
int  m4v_config(M4vDec* d, const u8* buf, int len);

/* Decodes one access unit. buf must be followed by MP_INPUT_PADDING readable bytes (zeroes).
 * Returns an M4V_FRAME_* value (>= 0) or an M4V_ERR_* value. When a picture is available
 * (I, P, REPEAT) *pic describes it; it stays valid until the next call. */
int  m4v_decode(M4vDec* d, const u8* buf, int len, M4vPicture* pic);

/* Forget the reference picture (call after seeking; decoding resumes at the next I-frame). */
void m4v_flush(M4vDec* d);

/* Size known (after m4v_config or the first decoded header): returns 1 and fills w/h. */
int  m4v_size(M4vDec* d, int* w, int* h);

/* Short text describing the last error / the stream (for the UI). */
const char* m4v_error_text(M4vDec* d);
const char* m4v_stream_info(M4vDec* d);
/* for "Analyze a file": stream parameters from the VOL (valid when haveVol) */
struct M4vParams { int haveVol, shortHeader, verid, quantType, resyncMarker, timeIncBits, width, height; };
void m4v_params(M4vDec* d, M4vParams* p);

/* FNV-1a hash of a picture's visible pixels (Y, then U and V), chained through h.
   Start with h = 2166136261. Used to check that the phone decodes exactly like the PC. */
u32 m4v_picture_hash(const M4vPicture* p, u32 h);

#endif
