# MP4 Player 9300

A native (Symbian C++) video player for the **Nokia 9300 / 9500** (Series 80 2.0, Symbian 7.0s EKA1),
built with the **Series 80 Developer Platform 2.0 SDK** (`S80_DP_2_0_SDK.zip`).

The phone has no video decoder an app can use: the S80 2.0 SDK ships `CVideoPlayerUtility`, but the
platform has no MMF video *playback* controller (only the camcorder's recorder and audio controllers).
So the player brings its own:

- **Video:** its own MP4/3GP/MOV demuxer and software decoders for **MPEG-4 Part 2 (Simple
  Profile), H.263** and (since 1.8) **H.264 Baseline**, converted straight into a `CFbsBitmap`
  (EColor64K / 4K / 16M) and scaled to the screen.
- **Audio:** the phone's MMF audio controller plays the same file (AAC, AMR-NB/WB). Its position is
  also used to keep the video clock in sync.
- **Other video codecs** (e.g. H.264 Main/High with CABAC) go to the system `CVideoPlayerUtility` in
  case the phone has a controller for them. If it doesn't, the sound plays on its own and a message
  says why.

## Status

- **1.0**: first version. The decoder core is verified on a PC. Every decoded frame is **bit-identical
  to ffmpeg** on the test set: I/P-VOPs, 4MV, MPEG quantisation matrices, resync markers, odd sizes,
  Xvid, adaptive quant, large motion vectors, and H.263 sub-QCIF/QCIF/4CIF. It has also survived
  ~5,000 decodes of randomly corrupted files under AddressSanitizer. The app builds cleanly with the
  EKA1 toolchain (no warnings, no writable static data).
- **First test on a 9300** (1.0): `sample_320x180_mpeg4_aac.mp4` plays. Over about 15 s it showed 159
  frames and skipped 43 (decoded but not drawn while catching up), with no errors. That's about
  12 of 15 fps, so the phone is just short of real time at this size.
- **1.1**: the File information screen (I) shows the time per frame for decoding, colour conversion
  and drawing, measured on the phone. Fit zoom no longer stretches a video that almost fits, so the
  fast unscaled colour conversion is used. Full-pel motion compensation copies rows with `Mem::Copy`.
- **1.2**: the 1.1 test showed a 463×174 video area: the status bar ate the height, so 320×180 was
  scaled down, the slow path (196 shown / 100 skipped). The status is now a **136 px column on the
  right**, which leaves 327×200 for the video, so 320×200 and 320×180 play 1:1. Fit zoom crops a video
  that is at most 12 % too big instead of scaling it. Timing works with any `FastCounter` rate (falls
  back to the 1/64 s tick) and is shown first in File information.
  `bin/mp4player_O2.sis` is the same app built with `gcc -O2` (`OPT=O2 ./build.sh`) to compare speed
  against the SDK's default `-O`.
- **1.2 on the phone**: 320×180 sample, 294 shown / 6 skipped (full 15 fps). Per frame: decode
  28.4 ms, colour 26.6 ms, draw 8.3 ms, together 63 of the 66.7 ms available. The phone's
  `FastCounter` runs at 64 Hz (same as the tick), so the averages come from tick sums.
- **1.3**: faster 1:1 colour conversion for 64K/4K colours. Its tables shrink from ~11 KB to ~3.5 KB,
  so they fit the ARM9's 8 KB data cache. It converts two rows per chroma row and writes two pixels
  per 32-bit store. Output is identical to 1.2 (checked on 2,400 conversions, including crops and
  odd sizes).
- **1.2 `-O2` on the phone**: 300 shown / 0 skipped; decode 23.1 ms (−19 %), colour 26.5, draw 4.3 ms.
  It worked fine, but the status panel flickered more.
- **1.4**: `-O2` is the default (`OPTION GCC -O2` in the .mmp; `OPT=O ./build.sh` builds the `-O`
  variant). The status panel is drawn into an off-screen bitmap and copied in one go. It's redrawn
  at most twice a second, and only when something on it changes, which removes the flicker. The
  File information screen is shorter so everything fits.
- **1.3 `-O` on the phone**: 299 / 1; decode 24.1, colour 25.7, draw 5.3 ms. The colour fix barely
  shows, but the per-step split is unreliable: the only timer ticks at 64 Hz, and playback wakes on
  those ticks, so the time can be attributed to the wrong step.
- **1.5**: **Speed test** (B, File menu). It times reading, decoding (150 frames back to back), colour
  conversion into the screen bitmap and into ordinary RAM, drawing, and a plain memory copy, each in
  a long loop, so the 64 Hz timer doesn't matter.
- **1.4 on the phone**: no flicker. 596 shown / 4 skipped over two plays. The picture froze briefly at
  0.5–1.5 s: the video clock started before the phone's audio really did, and the first sync
  jumped it back.
- **1.6**: when playback starts, the picture waits (up to 1.5 s) until the audio position moves, then
  the video clock starts from it. Later drifts under 600 ms are corrected by half at a time instead
  of jumping. The speed test also prints **Check**, a hash of the first 150 decoded frames. On the PC,
  `test/m4vtest` prints the same hash; for `samples/sample_320x180_mpeg4_aac.mp4` it's
  **56886216**. If the phone shows the same value, its decoding is bit-identical to ffmpeg.
- **1.6 on the phone**: starts cleanly. Check = 56886216 (bit-identical). Speed test, 320×180:
  read 1.2, decode 28.9, colour 23.4 ms (the same into the bitmap and into RAM, so memory isn't the
  problem), draw 7.5 ms, 112 KB copy 4.6 ms. Sound OK and in sync.
- **1.7**: found with `tools/armemu`, which runs the core as compiled by the phone's GCC 2.9 in the
  Unicorn ARM emulator, checks the output and counts instructions. GCC 2.9 has no 16-bit
  loads/stores (each `short` access becomes byte operations). It turns constant multiplies into
  shift/add chains, and it spills registers in bigger loops.
  - Colour conversion: two rows per chroma row in hand-written ARM (inline asm, GCC 2.9
    syntax), byte tables indexed by Y + chroma offset. 42.3 → **12.7 instructions per pixel**. Output
    within one 565 step of 1.6's exact conversion on ~4 % of pixels.
  - Decoder: the coefficient block and IDCT use `int` instead of `short`. The IDCT skips empty rows,
    has an exact shortcut for first-row-only blocks, and clears only the rows it used. Inter
    dequantisation touches only the coefficients read. 2.04 → **1.79 M instructions per frame** on
    the sample. Still bit-exact with ffmpeg on all test clips; the emulator gives check 56886216.

- **1.7 on the phone**: speed test, 320×180 MPEG-4: read 1.2, decode 25.5, colour 11.9 (bitmap) /
  11.7 (RAM), draw 7.5 ms.
- **1.8**: **H.264 Baseline** with h264bsd (`core/h264`, see below). Every frame matches ffmpeg on 21
  H.264 test clips (426×240, 320×240, 320×180, 176×144, 400×224; slices, 16 reference frames,
  intra refresh, constrained intra, deblocking off, noise, very low and very high quality, Main
  profile without CABAC). The phone's compiler gives the same output in the emulator. About 840
  decodes of corrupted files ran under AddressSanitizer/UBSan without errors. The player says why
  it can't play H.264 High profile, or Main profile with CABAC. Scaled colour conversion has a
  fast path (24 instead of ~45 instructions per pixel). Expected on the phone, from emulator
  instruction counts (~14 ns per instruction measured with MPEG-4): 320×180 H.264 ≈ 32 ms per
  frame (15 fps), 426×240 ≈ 60 ms per frame plus scaling (≈ 10–12 fps, with skipped frames).

- **1.9**: 1:1 zoom no longer shrinks a video that's too big: it shows a 1:1 cut-out, at the top,
  middle or bottom (key C), using the fast unscaled colour conversion (12.8 instead of 24
  instructions per pixel). Checked in the emulator against the PC output. E.g. 426×240 H.264 in
  full screen shows 426×200 of it; in the window, 326×200.
- **1.9 on the phone**: H.264 bit-exact (checks 65a22c84 and c41dcd1c match the PC). Speed test:
  320×180 H.264 decode 45.4 ms per frame (MPEG-4: 25.5), so it plays at 15 fps; 426×240 decode
  75 ms, too slow for 15 fps (frames get skipped). 1:1 crop saves about 2 ms of colour conversion.
  No sound on H.264 files: the phone's MP4 audio player refuses any file with H.264 video in it,
  even with ordinary AAC sound. After full screen the window was too wide.
- **1.10**: sound for H.264 files. The player copies the sound track into an audio-only MP4 (same
  AAC/AMR data, same sample tables, only new chunk offsets) and plays that: `System\Temp\
  mp4player_sound.mp4` on the video's drive, or on C: when there is no room. Progress shows in the
  status panel; Space plays at once without sound. The last copy is kept (with a small .txt
  noting which video it belongs to), so playing the same video again starts immediately. The
  copy also kicks in when the audio player refuses any other file. Checked on the PC: the copied
  sound is packet-for-packet identical to the original (ffmpeg md5) for AAC and AMR, and about
  44,000 copies from damaged files ran under AddressSanitizer/UBSan without errors (which also found and
  fixed integer overflows in the demuxer's table size checks). Window size after full screen fixed.
  The copy reads through the whole file (audio comes in small pieces between the video frames), so
  it takes about as long as reading the file once: well under a second for a short clip, maybe
  half a minute for a long film.

- **1.11**: ARM assembler for the H.264 decoder's hot loops (ARMv4T, written for the 9300's ARM9):
  deblocking filters, 6-tap luma and bilinear chroma interpolation, full-pel block copy, the 4x4
  inverse transform with dequantisation, and writing the macroblock into the picture. Each routine
  is checked against the C code on random data in the ARM emulator, and 14 test streams decode
  bit-exactly (same checks as on the PC). Emulator instructions per frame, 150 frames:
  320×180 sample 1.90 M (was about 2.3 M), 426×240 sample 3.24 M. New **fast H.264** mode (key Q,
  View menu, remembered): skips the deblocking filter, 1.32 M / 2.33 M instructions per frame
  (about 30% less). The price is a slightly blocky picture that drifts a little from the correct
  one until the next key frame: barely visible at the converter's bitrates (48 dB from the correct
  pictures for the samples), more visible on low-bitrate video (32–35 dB).

- **1.11 on the phone**: decode 36.6 ms per frame at 320×180 (1.9: 45.4), 60.0 ms at 426×240
  (1.9: 75). About 19 ns per emulator instruction, as predicted. Sound plays for H.264 files
  through the audio-only copy (1.10) and stays in sync at 320×180; at 426×240 the picture lagged
  behind the sound.
- **1.12**: a video the phone can't decode at full speed now stays in sync: while the picture is
  more than 40 ms behind the sound, frames are decoded without being shown (they can't be
  skipped: the next frames are predicted from them), and a picture is shown when it has caught
  up, or at least every 250 ms. Before, it showed after every few frames and fell up to 1.5 s
  behind. Fast H.264 has three settings (Q): **auto** (default: fast for videos bigger than
  320×240), always best, always fast. The speed test marks results decoded in fast mode.

- **1.12 on the phone**: 426×240 in auto (fast) mode: decode 47.6 ms, colour 16.6, draw 7.2: pretty
  smooth and in sync, with an occasional hitch (71 ms per frame against 67 for 15 fps). In best
  mode (decode 60.4 ms) it still fell behind the sound: showing a picture every 250 ms cost more
  than the 6 ms per frame that decoding leaves.
- **1.13**: while behind, the player now yields to the keys every 250 ms without drawing, and
  shows a picture only once it has caught up (or at least once a second), so even a video that
  barely decodes in real time stays in sync.

- **1.14**: **direct drawing**. While playing, the colour conversion writes straight into the
  screen memory instead of a bitmap that the window server then copies (the 7–9 ms "draw" step).
  It uses Symbian's direct screen access (the window server tells the player when a menu or
  dialog covers the picture; then it draws the old way). The screen memory layout is found by a
  self-test the first time: 16 pixels are drawn with the screen's graphics context and looked
  for in the memory (giving the offset and the line length), then 8 pixels are written into the
  memory and read back through the screen device. If the test fails, or crashed the player (a
  marker is saved before it runs), the player keeps drawing the old way. Key D (View menu)
  switches it off and on (and repeats a failed test); the speed test reports the time for
  "colour + show" this way, or why it isn't used.

- **1.14 on the phone**: direct drawing works in full screen: 320×180 "colour + show" 12.5 ms (was
  11.7 + 7.8). 426×240 fit in full screen took 39 ms: the picture started at an odd x (143), so
  the screen lines weren't word aligned and the slow generic conversion ran. In the window the
  window server reported the picture as partly covered.
- **1.15**: the picture always starts at an even x (word aligned); a picture the window server
  reports as covered gives up direct access for 2 s (and the speed test shows the free area).

- **1.15 on the phone**: crashed (KERN-EXEC 3) when playing after a restart: the saved "test
  passed" skipped the self-test, so the screen address was never found (NULL).
- **1.16** (2.6 before the renumbering; 1.10–1.15 were 2.0–2.5): the self-test runs once in every run of the player (the saved result only remembers a
  failure or a crash).

- **1.16 on the phone**: direct drawing in full screen: 426×240 (fast H.264, fit, 354×200)
  "colour + show" 20.8 ms; 320×180 12.5 ms. In the window: "picture partly covered (picture
  2,10-322,190, free 92,0-555,200)": the normal window starts at x = 92 on the screen, and the
  picture's screen position was computed without that offset.
- **1.17**: screen position = window coordinates + the window's position on the screen (also
  for the phone's own video player, which got the same wrong rectangle).

- **1.17 on the phone**: direct drawing in the window works too: 320×180 "colour + show" 12.5 ms,
  426×240 (fast H.264, 326×184) 17.9 ms; smoother than before with some hitches; menus over the
  picture fine.
- **1.18**: **sound off** (M, Play menu, remembered): the audio player isn't opened at all (no
  AAC decoding, no sound copy for H.264), the video runs on its own clock. Switching it back on
  during playback opens the sound and starts it at the current position.

- **1.18 on the phone**: sound off made no difference (65.1 vs 64.9 ms decode in a long session;
  after a restart 51.7 ms, the same as the speed test's 51.0). 426×240 fast: direct drawing on
  showed 187 of 300 frames, off 136.
- **1.19**: more of the H.264 decoder in assembler or reworked, all bit-exact (14 streams, plus
  kernel tests against the C code on random data): the CAVLC residual block decoder (with the
  bit cache kept across the blocks of a macroblock), the bit reader, luma position j with its
  quarter-pel neighbours f/q in one pass, and a faster scan for emulation prevention bytes.
  Emulator instructions per frame (150 frames): 426×240 fast 2.23 M (was 2.33 M), 320×180
  1.85 M (was 1.90 M): about 4–5% less decoding time.

- **1.19 on the phone**: 426×240 fast: decode 48.8 ms (1.18: 51.0). Direct to screen 18.4 ms in
  fit, 14.8 ms in 1:1; played 1:1 with direct drawing: 250 of 300 frames shown.
- **1.20**: the info screen (I) and the speed test (B) start with all settings in short form, e.g.
  `[1.20 | Z 1:1 mid | F win | Q auto=fast | D on, used | M sound]`: version, zoom (and 1:1
  position), full screen or window, H.264 mode (auto shows what it chose), direct drawing (and
  whether the last picture used it), sound.

- **1.21**: a video whose decoding alone is slower than its frame time (e.g. 444×250 at 25 fps:
  40 ms per frame) can never catch up with the sound; since 1.13 it then showed only one picture
  a second. Now the player notices that (average decode time against the frame time) and shows
  a picture at least every 250 ms in that case. The info screen shows the frame time and says
  when decoding alone is too slow; it also says when the sound wasn't prepared because Space
  was pressed during "Preparing the sound".

- **1.22**: three adaptive things.
  - **Read-ahead**: the video file is read in 64 KB pieces, one file server request for many
    frames instead of one per frame (about 1 ms per frame in the speed test's "read").
  - **Auto H.264 mode measures** (Q auto): it starts with a guess by size (fast above 320×240),
    then every 45 frames compares decode + colour + draw with the frame time: too slow → fast,
    plenty of time left (decode × 1.35 + drawing under 85% of the frame time) → best again.
  - **Too slow video (K)**, when decoding alone takes longer than the frame time: *keep in time*
    draws every 500 ms and jumps to the next key frame when 0.5 s behind (instead of 1.5 s);
    *smooth* gives drawing about a quarter of the time (4 × the drawing time, 60–250 ms) and
    jumps at 1.5 s; *auto* keeps in time when there is sound and is smooth without. The settings
    line shows it as `K auto=sync` / `K auto=smooth`, plus `too slow` when that's the case.

- **1.23**: tried **decoding ahead** for H.264: up to 16 spare picture buffers in the decoder (a
  picture it no longer needed went into a ring of spares before its memory was reused), and
  the player decoded the next pictures into a queue in the time before the next one was due.
  On the PC, using every picture 7 pictures later gave the same check values on all 14 streams.
  The converter now makes a key frame every 4 s instead of 2 s.
- **1.23 on the phone**: 426×240 fast, 1:1, direct drawing, no sound: 274 of 300 shown with
  decoding ahead, 271 without (with sound, without it: 268). Too little for 3 MB and a more
  complicated playback loop (the phone has only ~5 ms per frame to fill the queue).
- **1.24**: decoding ahead removed again (as in 1.22, plus the converter's 4 s key frames).
- **1.25**: **Analyze a file** (File menu, A): pick a video and the player reports what it
  contains without playing it: container (brand, index at the start or end), video codec, size,
  fps, bitrate, frame sizes, key frame gaps, B-frames; for H.264 the avcC/SPS/PPS settings
  (profile, level, CAVLC/CABAC, reference frames, cropping, slice groups...) and, over the first
  60 frames, slices per frame, slice types, average QP and the deblocking filter; for MPEG-4 the
  VOL settings; the audio codec with its AAC config and bitrate. Then it decodes the first 60
  frames twice (best and fast) and compares the time with the frame time. The report is shown
  in pages and saved next to the video as `<name>_analysis.txt` (on C:\ if that fails), so the
  parameters can be reported and a similar test clip made on the PC.

## Install

`bin/mp4player.sis` (unsigned; Symbian 7.0s installs it after a warning). You can install it through
the kit's OTA server (`http://192.168.137.1:8000/`), Bluetooth or the memory card. Start
**MP4 Player** from the Desk.

Try the samples first:

| File | Video | Speed test check (B) |
|---|---|---|
| `samples/sample_320x180_mpeg4_aac.mp4` | MPEG-4 Part 2 + AAC | 56886216 |
| `samples/sample_320x180_h264_aac.mp4` | H.264 Baseline + AAC | 65A22C84 |
| `samples/sample_426x240_h264_aac.mp4` | H.264 Baseline 240p + AAC | C41DCD1C |
| `samples/sample_qcif_h263_amr.3gp` | H.263 + AMR | |

If the phone's speed test shows the same check value, its decoding is bit-identical to the PC.

## Use

| Key | Action |
|---|---|
| **Open** button, Ctrl+O, O | choose a file (`.mp4 .3gp .3g2 .m4v .mov .m4a`) |
| **Play/Pause** button, Space, Enter | play / pause |
| **Stop** button, S, Esc | stop (Esc leaves full screen first) |
| Left / Right | back / forward 10 s (Shift: 1 min); jumps to key frames |
| Up / Down | volume |
| F (Ctrl+F) | full screen 640×200 |
| Z (Ctrl+Z) | zoom: **fit** (whole picture) → **fill** (crops the edges so the picture fills the area, like SmartMovie's crop mode) → **1:1** (no scaling, the fastest: a video bigger than the area is cut, not shrunk) |
| C | 1:1: show the **top / middle / bottom** part of a video taller than the area (switches to 1:1 first). Zoom and position are remembered (`C:\System\Data\mp4player9300.ini`) |
| Q | H.264 **auto / best / fast**: fast skips the deblocking filter, about 30% less decoding work, a little blocky; auto uses it for videos bigger than 320×240 (remembered) |
| K | too slow video: auto / keep in time / smooth (see 1.22; remembered) |
| M | sound on/off: off leaves the phone's audio decoding out, a little more time for the video (remembered) |
| D | direct drawing on/off (see 1.14; remembered) |
| I (Ctrl+I) | file information: frames shown and skipped, time per step, codecs |
| B | speed test (see Status) |
| A | analyze a file: parameters and a decode test without playing it (see 1.25) |

Playback pauses when the app goes to the background. If the phone falls behind on a big video,
it skips frames and, when it's more than 1.5 s late, jumps to the next key frame.

## Which videos play

| | Plays | Doesn't play (video) |
|---|---|---|
| Container | MP4, 3GP, 3G2, MOV (moov at the start or the end) | fragmented MP4, files > 4 GB |
| Video | MPEG-4 Part 2 Simple Profile (`mp4v`; ffmpeg `mpeg4`, Xvid without B-frames, phone camera clips), H.263 baseline (`s263`), **H.264 Baseline / Constrained Baseline** (`avc1.42…`), H.264 Main without CABAC and B-frames | H.264 Main/High with CABAC or B-frames (most downloaded H.264, including YouTube's), HEVC; MPEG-4 ASP features: B-frames (skipped, see below), quarter-pel, GMC, interlace; data partitioning / RVLC |
| Audio | whatever the phone's audio controller takes: AAC-LC, AMR-NB, AMR-WB (from H.264 files through an audio-only copy, see 1.10) | — |

With B-frames the I/P frames still play, but motion gets jerky; re-encode with `-bf 0`.

**Size:** the 9300 has a 150 MHz ARM9. What worked for SmartMovie on the Communicators applies here
too: **at most 320×200** (200 is the full screen height, so it plays 1:1 in full screen, while 640 px
wide is too much for the CPU), **12–15 fps**, and roughly **1–2 MB per minute**. A lower bitrate also
means less work for the decoder. Bigger videos play with skipped frames.

### Converting videos

`tools\convert_for_9300.bat input.mkv [fit|crop] [mpeg4|h264] [output.mp4]` on Windows (or
`tools/convert_for_9300.sh` on Linux; options in any order) needs ffmpeg:

- `mpeg4` (default) is the fastest to decode on the phone. `h264` makes H.264 Baseline at
  160 kbit/s: a better picture for the size, but about 1.5× the decoding work.

- `fit` (default) keeps the whole picture inside 320×200: 16:9 becomes 320×180, 4:3 becomes 266×200.
- `crop` fills 320×200 by cutting off the sides of 16:9 video, or the top and bottom of 4:3 video.
  The player's **fill** zoom does the same at playback time.

Both run MPEG-4 Part 2 without B-frames at 15 fps and about 200 kbit/s, with a key frame every 4 s (fewer slow-to-decode key frames; a seek lands up to 4 s early)
so seeking stays precise. Sound is AAC mono at 22 kHz and 32 kbit/s, giving about 1.5–2 MB per
minute. For better quality, raise `-b:v` (e.g. 300k). To make the phone's job easier, lower it, or
change `fps=15` to `fps=12`. ffmpeg asks before overwriting an existing output file.

## How it works

```
core/        portable C-style C++ (builds with GCC 2.9 for the phone and g++ on a PC)
  mp4demux   moov parser -> sample table (offset, size, time, key) of the video track; audio info;
             audio-only copy of the sound track (ftyp + mvhd + sound trak + packed mdat)
  m4vdec     MPEG-4 Part 2 SP + H.263 decoder (VOL/VOP/GOB headers, AC/DC prediction, 1MV/4MV,
             half-pel MC with rounding control, H.263/MPEG quant, video packets, concealment)
  idct       8x8 integer IDCT (same algorithm as ffmpeg's simple_idct, so the output is bit-exact)
  yuv2rgb    BT.601 YUV 4:2:0 -> RGB565/444/888 with nearest-neighbour scaling (1:1 path in ARM asm)
  viddec     one interface for both decoders (the engine doesn't care which codec it is)
  h264/      H.264 Baseline: h264bsd (Apache 2.0; Android's software decoder, via
             github.com/oneam/h264bsd commit 42bcb5d) + h264dec.c (avcC, length-prefixed NAL units,
             cropping). Changes for the phone: no C library (h264port.h/.cpp), debug counters
             removed (no writable static data), the ~2 KB macroblock layer no longer cleared for
             every macroblock (blocks are zeroed when used), inline row copy in full-pel motion
             compensation. All changes are marked "MP4 Player 9300".
  m4vtables  VLC/scan/quant tables (generated by tools/gentables.py)
src/
  engine     (h264bsd allocates with User::Alloc; the engine switches to the decoder heap with
             User::SwitchHeap around decoder calls)
             CActive playback loop: reads samples with RFile, decodes, converts, shows; clock from
             User::TickCount corrected by the audio position; CMdaAudioPlayerUtility for the sound;
             CVideoPlayerUtility fallback; the decoder gets its own chunk heap (up to 12 MB)
  view       video bitmap, status bar (state, time, progress, volume, file name), keys
  app        Eikon app, CBA, menus, CCknOpenFileDialog filtered to video files, file info
aif/         Desk icon (25x20 and 64x50) and caption; registers video/mp4, video/3gpp with low
             priority, so the File manager can open videos with it when no other app takes them
```

EKA1 apps are DLLs, so there is **no writable static data**: all tables are `static const` without
pointers, and the VLC lookup tables are built at run time in the decoder context.

UID `0x0F5A9301` (test range; Net Helper is `0x0F5A9300`).

## Build (Linux)

```sh
./build.sh    # needs ~/sym from the main repo's setup_eka1_toolchain.sh + setup_s80_sdk.sh
```

The output is `bin/mp4player.sis`. Like Net Helper, it uses the S80 2.0 SDK headers and libraries with
the S60 2.1 SDK's (patched) build tools. On a system without apt access, the setup also needs
`flex`, `cabextract` and `unshield` built from source.

## Test (PC)

```sh
tools/runtests.sh     # builds test/m4vtest, encodes test clips with ffmpeg, compares every frame
```

`tools/build_host.sh` builds `test/m4vtest`; `test/m4vtest in.mp4 out.yuv` decodes a file (any of
the three codecs) to raw I420 and prints the check value. `test/fuzz.cpp` is the corruption test
(build it with `-fsanitize=address,undefined`). `test/audiocopy.cpp in.mp4 out.mp4` writes the
audio-only copy; `test/fuzzaudio.cpp` is its corruption test.
`MAIN=$PWD/test/analyze.cpp tools/build_host.sh /tmp/analyze` builds the PC version of the
analysis data (`/tmp/analyze in.mp4`).

## ARM emulator (tools/armemu)

`tools/armemu/build.sh` compiles the core with the phone's compiler and flags into a flat binary.
`run.py decode|profile|colour|h264|h264prof FILE` runs it in Unicorn (built from source:
https://github.com/unicorn-engine/unicorn). It prints the decode checksum, instructions per frame,
a per-function profile, or instructions per pixel for the colour conversion. On the phone, about
2.1 cycles per instruction for decoding (28.9 ms for 2.04 M instructions at 150 MHz).

## License notes

The VLC tables (`core/m4vtables.h`) and the IDCT (`core/idct.cpp`) are derived from FFmpeg's
libavcodec (LGPL-2.1-or-later). The tables are data from ISO/IEC 14496-2 and ITU-T H.263.
`core/h264` is h264bsd, Apache License 2.0 (`core/h264/LICENSE-h264bsd.md`).
