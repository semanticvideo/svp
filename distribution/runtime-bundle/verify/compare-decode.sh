#!/usr/bin/env bash
# Byte-compare a reference ffmpeg/ffprobe pair against a candidate pair using
# the invocation shapes SVP issues (see ffmpeg-svp-features.txt for the
# source files). Prints one TSV row per clip and shape:
#   clip  shape  result  sha256(reference)
# where result is "identical", "DIFFERENT", or "both-failed".
# Exit status is 1 if any row is not identical.
#
# Usage:
#   compare-decode.sh --ref-ffmpeg A --ref-ffprobe B \
#                     --test-ffmpeg C --test-ffprobe D --out DIR clip...
set -euo pipefail

REF_FF="" REF_FP="" TEST_FF="" TEST_FP="" OUT=""
while [ $# -gt 0 ]; do
  case "$1" in
    --ref-ffmpeg) REF_FF="$2"; shift 2 ;;
    --ref-ffprobe) REF_FP="$2"; shift 2 ;;
    --test-ffmpeg) TEST_FF="$2"; shift 2 ;;
    --test-ffprobe) TEST_FP="$2"; shift 2 ;;
    --out) OUT="$2"; shift 2 ;;
    --) shift; break ;;
    -*) echo "unknown argument: $1" >&2; exit 2 ;;
    *) break ;;
  esac
done
[ -n "$REF_FF" ] && [ -n "$REF_FP" ] && [ -n "$TEST_FF" ] && [ -n "$TEST_FP" ] && [ -n "$OUT" ] && [ $# -gt 0 ] \
  || { echo "usage: $0 --ref-ffmpeg A --ref-ffprobe B --test-ffmpeg C --test-ffprobe D --out DIR clip..." >&2; exit 2; }

# Coverage policy for seeks: one frame near the start, middle, and end of the
# clip (fractions of the probed duration), so keyframe and non-keyframe seek
# targets in different GOPs are both exercised.
SEEK_FRACTIONS="0.1 0.5 0.9"
# SVP's OCR raster cap: frames wider than this are scaled down for OCR.
OCR_MAX_WIDTH=1920
# Visual-entity style sampling: frames per second and output width.
SAMPLE_FPS=2
SAMPLE_WIDTH=640
# Evidence-crop JPEG quality scale (-q:v), a mid-range SVP crop quality.
CROP_QSCALE=3

failures=0
even() { echo $(( ($1 / 2) * 2 )); }

# run_pair CLIP SHAPE EXT TEMPLATE: TEMPLATE is a shell command using $FF, $FP, and $O
# The template is evaluated once per side with FF/FP/O set for that side.
run_pair() {
  local clip="$1" shape="$2" ext="$3"; shift 3
  local side ff fp o status_ref status_test
  for side in ref test; do
    if [ "$side" = ref ]; then ff="$REF_FF"; fp="$REF_FP"; else ff="$TEST_FF"; fp="$TEST_FP"; fi
    o="$OUT/$clip/$side/$shape.$ext"
    mkdir -p "$(dirname "$o")"
    rm -f "$o"
    set +e
    FF="$ff" FP="$fp" O="$o" bash -c "$*" 2> "$o.stderr"
    local st=$?
    set -e
    if [ "$side" = ref ]; then status_ref=$st; else status_test=$st; fi
  done
  local a="$OUT/$clip/ref/$shape.$ext" b="$OUT/$clip/test/$shape.$ext" result digest="-"
  if [ "$status_ref" != 0 ] && [ "$status_test" != 0 ]; then
    result="both-failed"
  elif [ "$status_ref" = 0 ] && [ "$status_test" = 0 ] && [ -f "$a" ] && [ -f "$b" ] && cmp -s "$a" "$b"; then
    result="identical"; digest="$(shasum -a 256 "$a" | awk '{print $1}')"
  else
    result="DIFFERENT"; failures=$((failures + 1))
  fi
  printf '%s\t%s\t%s\t%s\n' "$clip" "$shape" "$result" "$digest"
}

for src in "$@"; do
  clip="$(basename "$src")"
  q="$(printf '%q' "$src")"
  probe="$("$REF_FP" -v error -print_format json -show_format -show_streams "$src")"
  duration="$(jq -r '.format.duration' <<<"$probe")"
  width="$(jq -r '[.streams[] | select(.codec_type=="video")][0].width // empty' <<<"$probe")"
  height="$(jq -r '[.streams[] | select(.codec_type=="video")][0].height // empty' <<<"$probe")"
  audio_indexes="$(jq -r '.streams[] | select(.codec_type=="audio") | .index' <<<"$probe")"

  # media_probe.cpp
  run_pair "$clip" probe json '"$FP" -v error -print_format json -show_format -show_streams '"$q"' > "$O"'
  # Whole-clip decoder identity: every decoded video and audio frame.
  run_pair "$clip" decode_all framemd5 '"$FF" -v error -i '"$q"' -map 0:V? -map 0:a? -f framemd5 "$O"'

  if [ -n "$width" ]; then
    ocr_w="$width"; ocr_h="$height"
    if [ "$width" -gt "$OCR_MAX_WIDTH" ]; then
      ocr_w="$OCR_MAX_WIDTH"; ocr_h="$(even $(( height * OCR_MAX_WIDTH / width )))"
    fi
    sample_h="$(even $(( height * SAMPLE_WIDTH / width )))"
    cw="$(even $(( width / 3 )))"; ch="$(even $(( height / 4 )))"; cx="$(even $(( width / 3 )))"; cy="$(even $(( height / 2 )))"
    for frac in $SEEK_FRACTIONS; do
      t="$(awk -v d="$duration" -v f="$frac" 'BEGIN{printf "%.6f", d*f}')"
      # canonical_frame_input.cpp: one-frame seek at the canonical raster.
      run_pair "$clip" "canonical_rgb24_at_$frac" rgb '"$FF" -v error -ss '"$t"' -i '"$q"' -vf scale='"$width:$height"' -vframes 1 -f rawvideo -pix_fmt rgb24 pipe:1 > "$O"'
      # OCR raster (<= OCR_MAX_WIDTH wide).
      run_pair "$clip" "ocr_rgb24_at_$frac" rgb '"$FF" -v error -ss '"$t"' -i '"$q"' -vf scale='"$ocr_w:$ocr_h"' -vframes 1 -f rawvideo -pix_fmt rgb24 pipe:1 > "$O"'
      # evidence_crop.cpp: crop + scale + mjpeg, then crop_hardening.cpp decode.
      run_pair "$clip" "crop_jpeg_at_$frac" jpg '"$FF" -v error -ss '"$t"' -i '"$q"' -vf "crop='"$cw:$ch:$cx:$cy"',scale='"$(( cw * 2 )):$(( ch * 2 ))"',format=yuvj420p" -vframes 1 -c:v mjpeg -q:v '"$CROP_QSCALE"' -y "$O"'
      crop_ref="$OUT/$clip/ref/crop_jpeg_at_$frac.jpg"
      [ -f "$crop_ref" ] && run_pair "$clip" "crop_decode_rgb24_at_$frac" rgb '"$FF" -v error -i '"$(printf '%q' "$crop_ref")"' -vf scale='"$cw:$ch"' -vframes 1 -f rawvideo -pix_fmt rgb24 pipe:1 > "$O"'
    done
    # visual_entity_frame_decoder.cpp: fps + scale over a span.
    run_pair "$clip" sampled_fps_rgb24 rgb '"$FF" -v error -ss 0.5 -i '"$q"' -t 4 -vf fps='"$SAMPLE_FPS"',scale='"$SAMPLE_WIDTH:$sample_h"' -frames:v 8 -f rawvideo -pix_fmt rgb24 pipe:1 > "$O"'
  fi

  first_audio=""
  for ai in $audio_indexes; do
    [ -n "$first_audio" ] || first_audio="$ai"
    # audio_extraction_plan.cpp: original stream to FLAC.
    run_pair "$clip" "original_stream_${ai}_flac" flac '"$FF" -hide_banner -nostdin -nostats -v error -y -i '"$q"' -map 0:'"$ai"' -vn -c:a flac "$O"'
    # Single-stream analysis WAV.
    run_pair "$clip" "analysis_${ai}_s16le" wav '"$FF" -hide_banner -nostdin -nostats -v error -y -i '"$q"' -map 0:'"$ai"' -vn -ac 1 -ar 16000 -c:a pcm_s16le "$O"'
    # Microphone analysis WAV with timeline normalization. SVP passes the
    # probed stream offset; a zero offset is used here, and the full SVP build
    # comparison covers real offsets.
    run_pair "$clip" "microphone_${ai}_s16le" wav '"$FF" -hide_banner -nostdin -nostats -v error -y -i '"$q"' -map 0:'"$ai"' -vn -af "asetpts=PTS-STARTPTS+0.000000/TB,aresample=async=1:first_pts=0" -ac 1 -ar 16000 -c:a pcm_s16le "$O"'
  done
  n_audio="$(printf '%s\n' $audio_indexes | grep -c . || true)"
  if [ "$n_audio" -gt 1 ]; then
    graph=""; labels=""; k=0
    for ai in $audio_indexes; do
      graph="$graph[0:$ai]asetpts=PTS-STARTPTS+0.000000/TB,aresample=async=1:first_pts=0[mic$k];"
      labels="$labels[mic$k]"; k=$((k + 1))
    done
    graph="$graph${labels}amix=inputs=$n_audio:duration=longest:normalize=1[mixed]"
    run_pair "$clip" mixed_s16le wav '"$FF" -hide_banner -nostdin -nostats -v error -y -i '"$q"' -filter_complex "'"$graph"'" -map "[mixed]" -vn -ac 1 -ar 16000 -c:a pcm_s16le "$O"'
  fi
  if [ -n "$first_audio" ]; then
    wav_ref="$OUT/$clip/ref/analysis_${first_audio}_s16le.wav"
    flac_ref="$OUT/$clip/ref/original_stream_${first_audio}_flac.flac"
    # loudness_meter.cpp: ebur128 through ffprobe lavfi, on the same input file
    # for both sides so only the measuring binary differs.
    run_pair "$clip" loudness_ebur128 json '"$FP" -v error -f lavfi -i "amovie='"$wav_ref"':si=0,ebur128=metadata=1:peak=true" -show_frames -of json > "$O"'
    # spectrum_analyzer.cpp: decode FLAC to f32le.
    run_pair "$clip" spectrum_f32le f32 '"$FF" -v error -i '"$(printf '%q' "$flac_ref")"' -map 0:a:0 -f f32le -acodec pcm_f32le - > "$O"'
  fi
done

[ "$failures" = 0 ]
