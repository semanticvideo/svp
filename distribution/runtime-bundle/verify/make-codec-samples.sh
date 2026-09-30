#!/usr/bin/env bash
# Generate short synthetic clips that cover the source codecs SVP decodes,
# using a reference ffmpeg that has the encoders (for example Homebrew's GPL
# build). The runtime-bundle ffmpeg cannot make these itself: it ships no
# HEVC, VP9, AV1, Opus, or MP3 encoders.
#
# Usage: make-codec-samples.sh --encoder-ffmpeg /opt/homebrew/bin/ffmpeg --out DIR
#
# Each clip pairs one video codec with one audio codec so a single decode run
# covers both. The content (moving test pattern, distinct per-channel tones,
# and a second audio stream on one clip for the amix path) exercises motion,
# chroma, and multi-channel downmix rather than static frames.
set -euo pipefail

ENC_FFMPEG=""
OUT=""
# Clip length: long enough for several GOPs at the keyframe interval below
# and for mid-clip seeks; short enough to keep the comparison fast.
DURATION_S=6
KEYINT=48
SIZE=1280x720
# One clip is wider than SVP's 1920-pixel OCR raster cap so the OCR shape in
# compare-decode.sh actually downscales.
LARGE_SIZE=3840x2160
RATE=24

while [ $# -gt 0 ]; do
  case "$1" in
    --encoder-ffmpeg) ENC_FFMPEG="$2"; shift 2 ;;
    --out) OUT="$2"; shift 2 ;;
    *) echo "unknown argument: $1" >&2; exit 2 ;;
  esac
done
[ -x "$ENC_FFMPEG" ] && [ -n "$OUT" ] || { echo "usage: $0 --encoder-ffmpeg PATH --out DIR" >&2; exit 2; }
mkdir -p "$OUT"

video_src="testsrc2=size=$SIZE:rate=$RATE:duration=$DURATION_S"
large_video_src="testsrc2=size=$LARGE_SIZE:rate=$RATE:duration=$DURATION_S"
audio_src="sine=frequency=440:sample_rate=48000:duration=$DURATION_S"
audio_src_b="sine=frequency=660:sample_rate=48000:duration=$DURATION_S"
stereo="[1:a][2:a]amerge=inputs=2[st]"

# enc NAME [--large] ENCODER_ARGS...
enc() {
  local name="$1"; shift
  local vsrc="$video_src"
  if [ "${1:-}" = "--large" ]; then vsrc="$large_video_src"; shift; fi
  "$ENC_FFMPEG" -hide_banner -v error -y \
    -f lavfi -i "$vsrc" -f lavfi -i "$audio_src" -f lavfi -i "$audio_src_b" \
    -filter_complex "$stereo" "$@" "$OUT/$name"
  echo "$OUT/$name"
}

enc h264_aac.mp4       -map 0:v -map '[st]' -c:v libx264 -pix_fmt yuv420p -g $KEYINT -c:a aac -b:a 128k
enc hevc_aac.mp4       -map 0:v -map '[st]' -c:v libx265 -pix_fmt yuv420p -x265-params "keyint=$KEYINT:log-level=error" -tag:v hvc1 -c:a aac -b:a 128k
enc hevc10_aac.mov     -map 0:v -map '[st]' -c:v libx265 -pix_fmt yuv420p10le -x265-params "keyint=$KEYINT:log-level=error" -tag:v hvc1 -c:a aac -b:a 128k
enc prores_pcm.mov     -map 0:v -map '[st]' -c:v prores_ks -profile:v 2 -c:a pcm_s24le
enc vp9_opus.webm      -map 0:v -map '[st]' -c:v libvpx-vp9 -b:v 1M -g $KEYINT -deadline realtime -cpu-used 8 -c:a libopus -b:a 96k
enc av1_mp3.mkv        -map 0:v -map '[st]' -c:v libsvtav1 -preset 10 -g $KEYINT -svtav1-params log-level=1 -c:a libmp3lame -b:a 128k
enc av1_opus.mp4       -map 0:v -map '[st]' -c:v libsvtav1 -preset 10 -g $KEYINT -svtav1-params log-level=1 -c:a libopus -b:a 96k
# 4K 10-bit HEVC, the common phone-camera format; exercises the OCR downscale.
enc hevc10_4k_aac.mov --large -map 0:v -map '[st]' -c:v libx265 -preset ultrafast -pix_fmt yuv420p10le -x265-params "keyint=$KEYINT:log-level=error" -tag:v hvc1 -c:a aac -b:a 128k
# Two audio streams: the analysis path mixes them with amix.
enc h264_2xaac.mp4     -map 0:v -map '[st]' -map 1:a -c:v libx264 -pix_fmt yuv420p -g $KEYINT -c:a aac -b:a 128k
