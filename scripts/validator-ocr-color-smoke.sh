#!/usr/bin/env bash
set -euo pipefail

validator="${1:-build/tools/svp-validator/svp-validator}"

if [[ ! -x "$validator" ]]; then
  echo "validator executable not found: $validator" >&2
  exit 2
fi

workdir="$(mktemp -d "${TMPDIR:-/tmp}/svp-ocr-color-smoke.XXXXXX")"
trap 'rm -rf "$workdir"' EXIT

python3 - "$workdir" <<'PY'
import copy
import json
import pathlib
import sys
import zipfile

root = pathlib.Path(sys.argv[1])

manifest = {
    "svp_version": "1.0-rc.2",
    "package_id": "svp_ocr_color_smoke",
    "created_utc": "2026-06-19T00:00:00Z",
    "primary_media_id": "media_0001",
    "timebase": {
        "unit": "microseconds",
        "origin": "primary_presentation_start",
        "source_timebase_mode": "exact_rational",
        "rounding": "round_half_to_even",
    },
    "canonical_analysis_raster": {
        "width": 640,
        "height": 360,
        "derivation": "longest_display_dimension_640_preserve_dar",
        "source_display_width": 640,
        "source_display_height": 360,
        "source_display_aspect_ratio": "16:9",
        "rotation_applied": False,
        "pixel_aspect_ratio_applied": False,
        "coordinate_origin": "top_left",
        "normalized_coordinates": True,
    },
}

text_region = {
    "text_region_id": "text_region_000001",
    "observation_type": "text_detection",
    "start_us": 0,
    "end_us": 1000000,
    "frame_start": 0,
    "frame_end": 30,
    "bbox_norm": [0.1, 0.2, 0.3, 0.4],
    "bbox_px": [64, 72, 192, 144],
    "confidence": 0.9,
    "provenance_id": "processor_ocr_detector_0001",
}

text_observation = {
    "text_observation_id": "text_obs_000001",
    "text_region_id": "text_region_000001",
    "observation_type": "text_recognition",
    "raw_text": "SALE $9.99",
    "normalized_text": "sale 9.99",
    "confidence": 0.91,
    "provenance_id": "processor_ocr_recognizer_0001",
}

numeric_value = {
    "numeric_value_id": "numeric_value_000001",
    "text_observation_id": "text_obs_000001",
    "text_region_id": "text_region_000001",
    "raw_text": "$9.99",
    "normalized_text": "9.99",
    "number_kind": "decimal",
    "numeric_value": "9.99",
    "confidence": 0.88,
    "parse_rule": "svp-number-parser-v1",
    "provenance_id": "processor_numeric_parser_0001",
}

color_observation = {
    "color_observation_id": "color_obs_000001",
    "target_type": "shot",
    "target_id": "shot_000001",
    "sampling_basis": "keyframe_full_frame",
    "color_space": "svp_oklch_v1",
    "color_bucket_registry_version": "svp-color-buckets-v1",
    "bucket_coverage": {
        "orange": 0.7,
        "black": 0.1,
        "white": 0.1,
        "other": 0.1,
    },
    "dominant_bucket": "orange",
    "coverage_total": 1.0,
    "quality_score": 0.96,
    "provenance_id": "processor_color_quantizer_0001",
}

text_absence = {
    "schema_version": "svp-text-absence-v1",
    "ocr_required": True,
    "ocr_completed": True,
    "text_region_count": 1,
    "text_observation_count": 1,
    "numeric_value_count": 1,
    "reason": "text_detected",
    "provenance_id": "processor_ocr_detector_0001",
}

color_summary = {
    "schema_version": "svp-color-summary-v1",
    "color_observation_count": 1,
    "color_space": "svp_oklch_v1",
    "color_bucket_registry_version": "svp-color-buckets-v1",
    "percentage_sum_tolerance": 0.001,
    "provenance_id": "processor_color_quantizer_0001",
}

color_absence = {
    "schema_version": "svp-color-absence-v1",
    "color_required": True,
    "color_completed": True,
    "color_observation_count": 1,
    "reason": "color_observed",
    "provenance_id": "processor_color_quantizer_0001",
}

def write_package(name, mutate, text_regions_payload=None):
    region = copy.deepcopy(text_region)
    observation = copy.deepcopy(text_observation)
    numeric = copy.deepcopy(numeric_value)
    color = copy.deepcopy(color_observation)
    mutate(region, observation, numeric, color)

    package_path = root / f"{name}.svp"
    with zipfile.ZipFile(package_path, "w", compression=zipfile.ZIP_DEFLATED) as package:
        for directory in [
            "media/",
            "transcript/",
            "timeline/",
            "entities/",
            "spatial/",
            "text/",
            "colors/",
            "relationships/",
            "embeddings/",
            "index/",
            "provenance/",
        ]:
            package.writestr(directory, "")
        package.writestr("mimetype", "application/vnd.svp.package")
        package.writestr("manifest.json", json.dumps(manifest, separators=(",", ":")))
        package.writestr(
            "text/text_regions.jsonl",
            text_regions_payload
            if text_regions_payload is not None
            else json.dumps(region, separators=(",", ":")) + "\n",
        )
        package.writestr("text/text_observations.jsonl", json.dumps(observation, separators=(",", ":")) + "\n")
        package.writestr("text/numeric_values.jsonl", json.dumps(numeric, separators=(",", ":")) + "\n")
        package.writestr("text/text_absence.json", json.dumps(text_absence, separators=(",", ":")))
        package.writestr("colors/color_observations.jsonl", json.dumps(color, separators=(",", ":")) + "\n")
        package.writestr("colors/color_summary.json", json.dumps(color_summary, separators=(",", ":")))
        package.writestr("colors/color_absence.json", json.dumps(color_absence, separators=(",", ":")))

def no_change(region, observation, numeric, color):
    pass

def invalid_ocr(region, observation, numeric, color):
    region.pop("confidence")

def invalid_bucket_space(region, observation, numeric, color):
    color["color_space"] = "not_registered"
    color["bucket_coverage"]["magenta-ish"] = color["bucket_coverage"].pop("other")

def invalid_percentage_total(region, observation, numeric, color):
    color["bucket_coverage"]["other"] = 0.05
    color["coverage_total"] = 0.95

write_package("valid", no_change)
write_package("invalid-ocr", invalid_ocr)
write_package("invalid-bucket-space", invalid_bucket_space)
write_package("invalid-percentage-total", invalid_percentage_total)
write_package(
    "oversized-text-regions",
    no_change,
    text_regions_payload=" " * (32 * 1024 * 1024 + 1),
)
PY

run_validator() {
  local package="$1"
  local output="$2"
  local status=0
  "$validator" validate "$package" --json >"$output" || status=$?
  printf '%s' "$status"
}

expect_status() {
  local actual="$1"
  local expected="$2"
  local label="$3"
  if [[ "$actual" != "$expected" ]]; then
    echo "$label: expected exit $expected, got $actual" >&2
    exit 1
  fi
}

expect_code() {
  local output="$1"
  local code="$2"
  python3 - "$output" "$code" <<'PY'
import json
import sys

report = json.loads(open(sys.argv[1], encoding="utf-8").read())
codes = {finding["code"] for finding in report.get("errors", [])}
if sys.argv[2] not in codes:
    raise SystemExit(f"missing expected code {sys.argv[2]}; saw {sorted(codes)}")
PY
}

valid_report="$workdir/valid.json"
valid_status="$(run_validator "$workdir/valid.svp" "$valid_report")"
expect_status "$valid_status" "0" "valid package"

invalid_ocr_report="$workdir/invalid-ocr.json"
invalid_ocr_status="$(run_validator "$workdir/invalid-ocr.svp" "$invalid_ocr_report")"
expect_status "$invalid_ocr_status" "1" "invalid OCR package"
expect_code "$invalid_ocr_report" "ERR_TEXT_INVALID_REGION_RECORD"

invalid_bucket_space_report="$workdir/invalid-bucket-space.json"
invalid_bucket_space_status="$(run_validator "$workdir/invalid-bucket-space.svp" "$invalid_bucket_space_report")"
expect_status "$invalid_bucket_space_status" "1" "invalid color bucket/space package"
expect_code "$invalid_bucket_space_report" "ERR_COLOR_INVALID_BUCKET_ID"
expect_code "$invalid_bucket_space_report" "ERR_COLOR_INVALID_COLOR_SPACE"

invalid_percentage_report="$workdir/invalid-percentage-total.json"
invalid_percentage_status="$(run_validator "$workdir/invalid-percentage-total.svp" "$invalid_percentage_report")"
expect_status "$invalid_percentage_status" "1" "invalid color total package"
expect_code "$invalid_percentage_report" "ERR_COLOR_INVALID_PERCENTAGE_TOTAL"

oversized_report="$workdir/oversized-text-regions.json"
oversized_status="$(run_validator "$workdir/oversized-text-regions.svp" "$oversized_report")"
expect_status "$oversized_status" "1" "oversized text regions package"
expect_code "$oversized_report" "ERR_TEXT_INVALID_REGION_RECORD"

echo "OCR/color validator smoke checks passed."
