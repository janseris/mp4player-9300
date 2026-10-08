@echo off
rem Converts any video into a file MP4 Player 9300 plays well on the Nokia 9300 / 9500.
rem Settings follow what worked for SmartMovie on the Communicators: at most 320x200 (the full
rem screen height; 640 wide is too much for the 150 MHz CPU), 15 fps, ~1.5 MB per minute.
rem   mpeg4 (default): MPEG-4 Part 2 Simple Profile, no B-frames, ~200 kbit/s - the fastest to decode
rem   h264:            H.264 Baseline, ~160 kbit/s - better picture per MB, ~1.5x the decoding work
rem   AAC mono 22 kHz 32 kbit/s
rem Needs ffmpeg.exe in PATH (or next to this file).
rem usage: convert_for_9300.bat input [fit^|crop] [mpeg4^|h264] [output.mp4]   (options in any order)
rem   fit  (default) whole picture, e.g. 16:9 -> 320x180, 4:3 -> 266x200
rem   crop fills 320x200 by cutting off the sides (16:9) or top and bottom (4:3)
setlocal
if "%~1"=="" (
  echo usage: %~nx0 input [fit^|crop] [mpeg4^|h264] [output.mp4]
  exit /b 1
)
set "IN=%~1"
set "OUT=%~dpn1_9300.mp4"
set "MODE=fit"
set "CODEC=mpeg4"
shift
:args
if "%~1"=="" goto run
if /i "%~1"=="fit" (set "MODE=fit") else if /i "%~1"=="crop" (set "MODE=crop") else if /i "%~1"=="mpeg4" (set "CODEC=mpeg4") else if /i "%~1"=="h264" (set "CODEC=h264") else (set "OUT=%~1")
shift
goto args
:run
set "VF=scale=320:200:force_original_aspect_ratio=decrease:force_divisible_by=2,fps=15"
if /i "%MODE%"=="crop" set "VF=scale=320:200:force_original_aspect_ratio=increase,crop=320:200,fps=15"
set "V=-c:v mpeg4 -vtag mp4v -bf 0 -g 30 -b:v 200k -maxrate 400k -bufsize 800k"
if /i "%CODEC%"=="h264" set "V=-c:v libx264 -profile:v baseline -preset slow -bf 0 -g 30 -b:v 160k -maxrate 320k -bufsize 640k -pix_fmt yuv420p"
ffmpeg -i "%IN%" -map 0:v:0 -map 0:a:0? -vf "%VF%" %V% -c:a aac -ac 1 -ar 22050 -b:a 32k -movflags +faststart "%OUT%"
