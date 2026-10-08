Host tests for the decoder core (Linux, g++):

- `m4vtest.cpp`: decode the video track of an MP4/3GP (MPEG-4, H.263, H.264) to raw I420 (`m4vtest in.mp4 out.yuv`); build with `../tools/build_host.sh`
- `convtest.cpp`: decode frame N and write it through yuv2rgb as a PPM (`convtest in.mp4 out.ppm N W H`)
- `fuzz.cpp`: decode randomly corrupted copies of a file (`g++ -fsanitize=address,undefined ...`, `fuzz in.mp4 iterations seed`)

`../tools/runtests.sh` builds m4vtest and compares every frame with ffmpeg.
