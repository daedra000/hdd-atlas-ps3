#!/bin/sh
# data/music.ogg from the track in art/music (not in git: 42 MB): 48 kHz (the PS3's audio port
# rate: no resampling on the console), loudness-normalised quiet (-24 LUFS, so it sits under the
# UI's sounds), Ogg Vorbis about 64 kbps (stb_vorbis plays it). Needs ffmpeg.
#   sh tools/make-music.sh
set -e
cd "$(dirname "$0")/.."
ffmpeg -v error -y -i art/music/relaxingtime-relaxing-music-part-1-141198.mp3 \
	-af loudnorm=I=-24:TP=-3:LRA=11 -ar 48000 -ac 2 -c:a libvorbis -q:a 0 data/music.ogg
ls -la data/music.ogg
