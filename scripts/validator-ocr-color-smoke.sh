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
import sqlite3
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

HASH_ZERO = "blake3:" + "0" * 64
HASH_ONE = "blake3:" + "1" * 64
HASH_TWO = "blake3:" + "2" * 64
HASH_THREE = "blake3:" + "3" * 64
HASH_FOUR = "blake3:" + "4" * 64

INDEX_TABLES = [
    """
    CREATE TABLE svp_meta (
      key TEXT PRIMARY KEY,
      value TEXT NOT NULL
    )
    """,
    """
    CREATE TABLE objects (
      object_id TEXT PRIMARY KEY,
      object_type TEXT NOT NULL,
      start_us INTEGER,
      end_us INTEGER,
      json_path TEXT NOT NULL
    )
    """,
    """
    CREATE TABLE temporal_spans (
      object_id TEXT NOT NULL,
      object_type TEXT NOT NULL,
      start_us INTEGER NOT NULL,
      end_us INTEGER NOT NULL
    )
    """,
    """
    CREATE TABLE relationships (
      relationship_id TEXT PRIMARY KEY,
      relationship_type TEXT NOT NULL,
      source_id TEXT NOT NULL,
      target_id TEXT NOT NULL,
      start_us INTEGER NOT NULL,
      end_us INTEGER NOT NULL,
      confidence REAL NOT NULL
    )
    """,
    """
    CREATE TABLE text_fts (
      object_id TEXT,
      object_type TEXT,
      start_us INTEGER,
      end_us INTEGER,
      text TEXT
    )
    """,
    """
    CREATE TABLE vector_index (
      embedding BLOB,
      object_id TEXT,
      object_type TEXT,
      embedding_set_id TEXT,
      start_us INTEGER,
      end_us INTEGER
    )
    """,
    """
    CREATE TABLE binary_blocks (
      block_id TEXT PRIMARY KEY,
      block_type TEXT NOT NULL,
      block_file TEXT NOT NULL,
      block_offset INTEGER NOT NULL,
      block_length INTEGER NOT NULL,
      payload_offset INTEGER NOT NULL,
      uncompressed_size INTEGER NOT NULL,
      compressed_size INTEGER NOT NULL,
      extent_0 INTEGER NOT NULL,
      extent_1 INTEGER NOT NULL,
      extent_2 INTEGER NOT NULL,
      dtype INTEGER NOT NULL,
      start_frame INTEGER,
      frame_count INTEGER,
      start_us INTEGER,
      end_us INTEGER,
      payload_blake3 TEXT NOT NULL,
      header_blake3 TEXT NOT NULL,
      block_blake3 TEXT NOT NULL
    )
    """,
    """
    CREATE TABLE text_regions (
      text_region_id TEXT PRIMARY KEY
    )
    """,
    """
    CREATE TABLE text_observations (
      text_observation_id TEXT PRIMARY KEY
    )
    """,
    """
    CREATE TABLE numeric_values (
      numeric_value_id TEXT PRIMARY KEY
    )
    """,
    """
    CREATE TABLE color_observations (
      color_observation_id TEXT PRIMARY KEY
    )
    """,
    """
    CREATE TABLE color_bucket_coverage (
      color_observation_id TEXT NOT NULL,
      bucket_id TEXT NOT NULL,
      coverage REAL NOT NULL
    )
    """,
    """
    CREATE TABLE color_targets (
      color_observation_id TEXT NOT NULL,
      target_type TEXT NOT NULL,
      target_id TEXT NOT NULL
    )
    """,
]

def index_manifest(table_count):
    return {
        "schema_version": "svp-index-manifest-v1",
        "index_schema_version": "svp-index-v1",
        "sqlite_file": "index/index.sqlite",
        "sqlite_file_blake3": HASH_ZERO,
        "logical_row_stream_version": "svp-logical-row-stream-v1",
        "logical_rows_blake3": HASH_ONE,
        "table_count": table_count,
        "row_count": 0,
        "created_from": {
            "manifest_blake3": HASH_TWO,
            "binary_blocks_manifest_blake3": HASH_THREE,
            "embedding_sets_blake3": HASH_FOUR,
        },
    }

def create_index_bytes(missing_table=None):
    sqlite_path = root / f"index-{missing_table or 'valid'}.sqlite"
    if sqlite_path.exists():
        sqlite_path.unlink()

    connection = sqlite3.connect(sqlite_path)
    try:
        for statement in INDEX_TABLES:
            if missing_table and f"CREATE TABLE {missing_table} " in statement:
                continue
            connection.execute(statement)
        connection.commit()
        table_count = connection.execute(
            "SELECT count(*) FROM sqlite_schema WHERE type = 'table' AND name NOT LIKE 'sqlite_%'"
        ).fetchone()[0]
    finally:
        connection.close()

    data = sqlite_path.read_bytes()
    sqlite_path.unlink()
    return data, table_count

def write_package(name, mutate, text_regions_payload=None, index_case="valid"):
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
        if index_case == "missing_manifest":
            sqlite_bytes, table_count = create_index_bytes()
            package.writestr("index/index.sqlite", sqlite_bytes)
        elif index_case == "malformed_manifest":
            sqlite_bytes, table_count = create_index_bytes()
            package.writestr("index/index.sqlite", sqlite_bytes)
            package.writestr("index/index_manifest.json", "{not json")
        elif index_case == "empty_index_schema_version":
            sqlite_bytes, table_count = create_index_bytes()
            manifest_value = index_manifest(table_count)
            manifest_value["index_schema_version"] = ""
            package.writestr("index/index.sqlite", sqlite_bytes)
            package.writestr("index/index_manifest.json", json.dumps(manifest_value, separators=(",", ":")))
        elif index_case == "missing_sqlite":
            _, table_count = create_index_bytes()
            package.writestr("index/index_manifest.json", json.dumps(index_manifest(table_count), separators=(",", ":")))
        elif index_case == "unreadable_sqlite":
            package.writestr("index/index.sqlite", b"not sqlite")
            package.writestr("index/index_manifest.json", json.dumps(index_manifest(0), separators=(",", ":")))
        elif index_case == "missing_color_table":
            sqlite_bytes, table_count = create_index_bytes("color_bucket_coverage")
            package.writestr("index/index.sqlite", sqlite_bytes)
            package.writestr("index/index_manifest.json", json.dumps(index_manifest(table_count), separators=(",", ":")))
        else:
            sqlite_bytes, table_count = create_index_bytes()
            package.writestr("index/index.sqlite", sqlite_bytes)
            package.writestr("index/index_manifest.json", json.dumps(index_manifest(table_count), separators=(",", ":")))

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
write_package("missing-index-manifest", no_change, index_case="missing_manifest")
write_package("malformed-index-manifest", no_change, index_case="malformed_manifest")
write_package("empty-index-schema-version", no_change, index_case="empty_index_schema_version")
write_package("missing-index-sqlite", no_change, index_case="missing_sqlite")
write_package("unreadable-index-sqlite", no_change, index_case="unreadable_sqlite")
write_package("missing-color-index-table", no_change, index_case="missing_color_table")
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

missing_manifest_report="$workdir/missing-index-manifest.json"
missing_manifest_status="$(run_validator "$workdir/missing-index-manifest.svp" "$missing_manifest_report")"
expect_status "$missing_manifest_status" "1" "missing index manifest package"
expect_code "$missing_manifest_report" "ERR_CORE_INDEX_MANIFEST_INVALID"

malformed_manifest_report="$workdir/malformed-index-manifest.json"
malformed_manifest_status="$(run_validator "$workdir/malformed-index-manifest.svp" "$malformed_manifest_report")"
expect_status "$malformed_manifest_status" "1" "malformed index manifest package"
expect_code "$malformed_manifest_report" "ERR_CORE_INDEX_MANIFEST_INVALID"

empty_index_schema_version_report="$workdir/empty-index-schema-version.json"
empty_index_schema_version_status="$(run_validator "$workdir/empty-index-schema-version.svp" "$empty_index_schema_version_report")"
expect_status "$empty_index_schema_version_status" "1" "empty index schema version package"
expect_code "$empty_index_schema_version_report" "ERR_CORE_INDEX_MANIFEST_INVALID"

missing_sqlite_report="$workdir/missing-index-sqlite.json"
missing_sqlite_status="$(run_validator "$workdir/missing-index-sqlite.svp" "$missing_sqlite_report")"
expect_status "$missing_sqlite_status" "1" "missing SQLite index package"
expect_code "$missing_sqlite_report" "ERR_CORE_INDEX_SCHEMA_INVALID"

unreadable_sqlite_report="$workdir/unreadable-index-sqlite.json"
unreadable_sqlite_status="$(run_validator "$workdir/unreadable-index-sqlite.svp" "$unreadable_sqlite_report")"
expect_status "$unreadable_sqlite_status" "1" "unreadable SQLite index package"
expect_code "$unreadable_sqlite_report" "ERR_CORE_INDEX_SCHEMA_INVALID"

missing_color_table_report="$workdir/missing-color-index-table.json"
missing_color_table_status="$(run_validator "$workdir/missing-color-index-table.svp" "$missing_color_table_report")"
expect_status "$missing_color_table_status" "1" "missing color index table package"
expect_code "$missing_color_table_report" "ERR_CORE_INDEX_SCHEMA_INVALID"

echo "OCR/color validator smoke checks passed."
