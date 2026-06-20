#!/usr/bin/env bash
set -euo pipefail

# This script generates a baseline comparison report for OCR quality.
# Usage: ./scripts/generate-ocr-baseline-report.sh [path_to_video] [output_report_file]

VIDEO_PATH=${1:-"/Users/domesposito/Projects/samples/test-30.mp4"}
REPORT_FILE=${2:-"docs/ocr_baseline_report.md"}

echo "Generating OCR baseline report for $VIDEO_PATH"
echo "Report will be saved to $REPORT_FILE"

BUILD_DIR="build/baseline_run"
STAGING_DIR="$BUILD_DIR/staging"
OUT_JSON="$BUILD_DIR/out.json"

mkdir -p "$BUILD_DIR"
rm -rf "$STAGING_DIR" "$OUT_JSON"

# Build the project first to ensure builder is up to date
echo "Building project..."
make -C build -j10

echo "Running svp-builder..."
build/tools/svp-builder/svp-builder build \
  --model-cache /Users/domesposito/Projects/svp-model-cache \
  --tesseract /opt/homebrew/bin/tesseract \
  --out "$OUT_JSON" \
  --staging-dir "$STAGING_DIR" \
  "$VIDEO_PATH" \
  --stop-after package-skeleton > /dev/null 2>&1

echo "Parsing output..."
echo "# OCR Baseline Comparison Report" > "$REPORT_FILE"
echo "" >> "$REPORT_FILE"
echo "**Video:** $VIDEO_PATH" >> "$REPORT_FILE"
echo "**Date:** $(date)" >> "$REPORT_FILE"
echo "" >> "$REPORT_FILE"
echo "## Extracted Text Observations" >> "$REPORT_FILE"
echo "" >> "$REPORT_FILE"
echo "| Raw Text | Normalized Text | Confidence |" >> "$REPORT_FILE"
echo "|----------|-----------------|------------|" >> "$REPORT_FILE"

# Extract text observations using jq or grep
if command -v jq >/dev/null 2>&1; then
  # Try to parse text observations (since structure is deep, we use recursive search for text_observations array)
  jq -r '.. | .text_observations? | select(. != null) | .[] | "| \(.raw_text) | \(.normalized_text) | \(.confidence) |"' "$OUT_JSON" >> "$REPORT_FILE" || echo "Failed to parse with jq" >> "$REPORT_FILE"
else
  grep '"raw_text"' "$OUT_JSON" | sed 's/ *"raw_text": "\(.*\)",/| \1 |/' >> "$REPORT_FILE"
fi

echo "Report generated at $REPORT_FILE"
