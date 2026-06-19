#!/usr/bin/env python3
"""Generate Phase 04 RC2 OCR/color SVP fixtures."""

from __future__ import annotations

import argparse
import json
import sqlite3
import tempfile
import zipfile
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any


REPO_ROOT = Path(__file__).resolve().parents[2]
DEFAULT_OUTPUT_DIR = REPO_ROOT / "fixtures" / "phase-04-ocr-color"
FIXED_ZIP_TIME = (2026, 6, 19, 0, 0, 0)
MIMETYPE = b"application/vnd.svp+zip"

HASH_ZERO = "blake3:" + "0" * 64
HASH_ONE = "blake3:" + "1" * 64
HASH_TWO = "blake3:" + "2" * 64
HASH_THREE = "blake3:" + "3" * 64
HASH_FOUR = "blake3:" + "4" * 64


@dataclass(frozen=True)
class TextCase:
    regions: list[dict[str, Any]] = field(default_factory=list)
    observations: list[dict[str, Any]] = field(default_factory=list)
    numeric_values: list[dict[str, Any]] = field(default_factory=list)
    absence_reason: str = "no_text_detected"


@dataclass(frozen=True)
class ColorCase:
    observations: list[dict[str, Any]] = field(default_factory=list)
    absence_reason: str = "color_observed"


@dataclass(frozen=True)
class FixtureCase:
    name: str
    package_id: str
    text: TextCase
    colors: ColorCase


def json_bytes(value: Any) -> bytes:
    return json.dumps(value, sort_keys=True, separators=(",", ":")).encode("utf-8")


def jsonl_bytes(rows: list[dict[str, Any]]) -> bytes:
    if not rows:
        return b""
    return b"".join(json_bytes(row) + b"\n" for row in rows)


def manifest(package_id: str) -> dict[str, Any]:
    return {
        "canonical_analysis_raster": {
            "coordinate_origin": "top_left",
            "derivation": "longest_display_dimension_640_preserve_dar",
            "height": 360,
            "normalized_coordinates": True,
            "pixel_aspect_ratio_applied": False,
            "rotation_applied": False,
            "source_display_aspect_ratio": "16:9",
            "source_display_height": 360,
            "source_display_width": 640,
            "width": 640,
        },
        "created_utc": "2026-06-19T00:00:00Z",
        "package_id": package_id,
        "primary_media_id": "media_000001",
        "svp_version": "1.0-rc.2",
        "timebase": {
            "origin": "primary_presentation_start",
            "rounding": "round_half_to_even",
            "source_timebase_mode": "exact_rational",
            "unit": "microseconds",
        },
    }


def frame_record() -> dict[str, Any]:
    return {
        "end_us": 1000000,
        "frame_id": "frame_000001",
        "frame_index": 0,
        "start_us": 0,
    }


def shot_record() -> dict[str, Any]:
    return {
        "end_us": 1000000,
        "frame_end": 0,
        "frame_start": 0,
        "shot_id": "shot_000001",
        "start_us": 0,
    }


def scene_record() -> dict[str, Any]:
    return {
        "end_us": 1000000,
        "scene_id": "scene_000001",
        "shot_ids": ["shot_000001"],
        "start_us": 0,
    }


def text_region(
    region_id: str = "text_region_000001",
    foreground_id: str | None = None,
    background_id: str | None = None,
) -> dict[str, Any]:
    record = {
        "bbox_norm": [0.1, 0.2, 0.5, 0.32],
        "bbox_px": [64, 72, 320, 115],
        "confidence": 0.93,
        "end_us": 1000000,
        "frame_end": 0,
        "frame_start": 0,
        "observation_type": "text_detection",
        "provenance_id": "processor_ocr_detector_0001",
        "scene_id": "scene_000001",
        "shot_id": "shot_000001",
        "start_us": 0,
        "text_direction": "ltr",
        "text_region_id": region_id,
    }
    if foreground_id is not None:
        record["foreground_color_observation_id"] = foreground_id
    if background_id is not None:
        record["background_color_observation_id"] = background_id
    return record


def text_observation(
    raw_text: str,
    normalized_text: str,
    observation_id: str = "text_obs_000001",
    region_id: str = "text_region_000001",
    layout_class: str = "ui_text",
) -> dict[str, Any]:
    return {
        "confidence": 0.91,
        "layout_class": layout_class,
        "normalized_text": normalized_text,
        "observation_type": "text_recognition",
        "provenance_id": "processor_ocr_recognizer_0001",
        "raw_text": raw_text,
        "source_frame_ids": ["frame_000001"],
        "text_observation_id": observation_id,
        "text_region_id": region_id,
    }


def numeric_value(
    raw_text: str,
    normalized_text: str,
    value: str,
    value_id: str = "numeric_value_000001",
    observation_id: str = "text_obs_000001",
    region_id: str = "text_region_000001",
    number_kind: str = "decimal",
    unit: str | None = None,
) -> dict[str, Any]:
    record = {
        "confidence": 0.88,
        "normalized_text": normalized_text,
        "number_kind": number_kind,
        "numeric_value": value,
        "numeric_value_id": value_id,
        "parse_rule": "svp-number-parser-v1",
        "provenance_id": "processor_numeric_parser_0001",
        "raw_text": raw_text,
        "text_observation_id": observation_id,
        "text_region_id": region_id,
    }
    if unit is not None:
        record["unit"] = unit
    return record


def color_observation(
    observation_id: str,
    target_type: str,
    target_id: str,
    bucket_coverage: dict[str, float],
    dominant_bucket: str,
    sampling_basis: str,
) -> dict[str, Any]:
    return {
        "bucket_coverage": bucket_coverage,
        "color_bucket_registry_version": "svp-color-buckets-v1",
        "color_observation_id": observation_id,
        "color_space": "svp_oklch_v1",
        "coverage_total": round(sum(bucket_coverage.values()), 4),
        "dominant_bucket": dominant_bucket,
        "end_us": 1000000,
        "frame_ids": ["frame_000001"],
        "provenance_id": "processor_color_quantizer_0001",
        "quality_score": 0.96,
        "sampling_basis": sampling_basis,
        "start_us": 0,
        "target_id": target_id,
        "target_type": target_type,
    }


def coverage(bucket: str, amount: float) -> dict[str, float]:
    remainder = round(1.0 - amount, 4)
    if remainder == 0:
        return {bucket: 1.0}
    return {bucket: amount, "other": remainder}


def baseline_color_observations(bucket: str = "gray") -> list[dict[str, Any]]:
    return [
        color_observation(
            "color_obs_000001",
            "scene",
            "scene_000001",
            coverage(bucket, 0.82),
            bucket,
            "full_frame",
        ),
        color_observation(
            "color_obs_000002",
            "shot",
            "shot_000001",
            coverage(bucket, 0.82),
            bucket,
            "keyframe_full_frame",
        ),
    ]


def text_absence(text: TextCase) -> dict[str, Any]:
    return {
        "numeric_value_count": len(text.numeric_values),
        "ocr_completed": True,
        "ocr_required": True,
        "provenance_id": "processor_ocr_detector_0001",
        "reason": text.absence_reason,
        "schema_version": "svp-text-absence-v1",
        "text_observation_count": len(text.observations),
        "text_region_count": len(text.regions),
    }


def color_summary(colors: ColorCase) -> dict[str, Any]:
    return {
        "color_bucket_registry_version": "svp-color-buckets-v1",
        "color_observation_count": len(colors.observations),
        "color_space": "svp_oklch_v1",
        "percentage_sum_tolerance": 0.001,
        "provenance_id": "processor_color_quantizer_0001",
        "schema_version": "svp-color-summary-v1",
    }


def color_absence(colors: ColorCase) -> dict[str, Any]:
    return {
        "color_completed": True,
        "color_observation_count": len(colors.observations),
        "color_required": True,
        "provenance_id": "processor_color_quantizer_0001",
        "reason": colors.absence_reason,
        "schema_version": "svp-color-absence-v1",
    }


def processor_records() -> list[dict[str, Any]]:
    return [
        {
            "completed_utc": "2026-06-19T00:00:00Z",
            "id": "processor_ocr_detector_0001",
            "input_refs": ["media/original/source_000.txt"],
            "name": "svp-fixture-ocr-detector",
            "output_refs": ["text/text_regions.jsonl", "text/text_absence.json"],
            "started_utc": "2026-06-19T00:00:00Z",
            "version": "phase04-fixture-v1",
        },
        {
            "completed_utc": "2026-06-19T00:00:00Z",
            "id": "processor_ocr_recognizer_0001",
            "input_refs": ["text/text_regions.jsonl"],
            "name": "svp-fixture-ocr-recognizer",
            "output_refs": ["text/text_observations.jsonl"],
            "started_utc": "2026-06-19T00:00:00Z",
            "version": "phase04-fixture-v1",
        },
        {
            "completed_utc": "2026-06-19T00:00:00Z",
            "id": "processor_numeric_parser_0001",
            "input_refs": ["text/text_observations.jsonl"],
            "name": "svp-fixture-numeric-parser",
            "output_refs": ["text/numeric_values.jsonl"],
            "started_utc": "2026-06-19T00:00:00Z",
            "version": "phase04-fixture-v1",
        },
        {
            "completed_utc": "2026-06-19T00:00:00Z",
            "id": "processor_color_quantizer_0001",
            "input_refs": ["timeline/frames.jsonl"],
            "name": "svp-fixture-color-quantizer",
            "output_refs": [
                "colors/color_observations.jsonl",
                "colors/color_summary.json",
                "colors/color_absence.json",
            ],
            "started_utc": "2026-06-19T00:00:00Z",
            "version": "phase04-fixture-v1",
        },
    ]


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
      text_region_id TEXT PRIMARY KEY,
      start_us INTEGER,
      end_us INTEGER,
      shot_id TEXT,
      scene_id TEXT
    )
    """,
    """
    CREATE TABLE text_observations (
      text_observation_id TEXT PRIMARY KEY,
      text_region_id TEXT NOT NULL,
      raw_text TEXT NOT NULL,
      normalized_text TEXT NOT NULL,
      layout_class TEXT
    )
    """,
    """
    CREATE TABLE numeric_values (
      numeric_value_id TEXT PRIMARY KEY,
      text_observation_id TEXT NOT NULL,
      text_region_id TEXT NOT NULL,
      numeric_value TEXT NOT NULL,
      raw_text TEXT NOT NULL,
      normalized_text TEXT NOT NULL
    )
    """,
    """
    CREATE TABLE color_observations (
      color_observation_id TEXT PRIMARY KEY,
      target_type TEXT NOT NULL,
      target_id TEXT NOT NULL,
      dominant_bucket TEXT NOT NULL
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


def insert_index_rows(connection: sqlite3.Connection, fixture: FixtureCase) -> None:
    connection.execute(
        "INSERT INTO svp_meta(key, value) VALUES (?, ?)",
        ("fixture_package_id", fixture.package_id),
    )

    object_rows = [
        ("frame_000001", "frame", 0, 1000000, "/timeline/frames.jsonl:1"),
        ("shot_000001", "shot", 0, 1000000, "/timeline/shots.jsonl:1"),
        ("scene_000001", "scene", 0, 1000000, "/timeline/scenes.jsonl:1"),
    ]
    for region in fixture.text.regions:
        object_rows.append(
            (
                region["text_region_id"],
                "text_region",
                region["start_us"],
                region["end_us"],
                "/text/text_regions.jsonl:1",
            )
        )
    for observation in fixture.text.observations:
        object_rows.append(
            (
                observation["text_observation_id"],
                "text_observation",
                0,
                1000000,
                "/text/text_observations.jsonl:1",
            )
        )
    for numeric in fixture.text.numeric_values:
        object_rows.append(
            (
                numeric["numeric_value_id"],
                "numeric_value",
                0,
                1000000,
                "/text/numeric_values.jsonl:1",
            )
        )
    for color in fixture.colors.observations:
        object_rows.append(
            (
                color["color_observation_id"],
                "color_observation",
                color.get("start_us"),
                color.get("end_us"),
                "/colors/color_observations.jsonl:1",
            )
        )

    connection.executemany(
        "INSERT INTO objects(object_id, object_type, start_us, end_us, json_path) "
        "VALUES (?, ?, ?, ?, ?)",
        object_rows,
    )
    connection.executemany(
        "INSERT INTO temporal_spans(object_id, object_type, start_us, end_us) "
        "VALUES (?, ?, ?, ?)",
        [(row[0], row[1], row[2] or 0, row[3] or 0) for row in object_rows],
    )

    connection.executemany(
        "INSERT INTO text_regions(text_region_id, start_us, end_us, shot_id, scene_id) "
        "VALUES (?, ?, ?, ?, ?)",
        [
            (
                region["text_region_id"],
                region["start_us"],
                region["end_us"],
                region.get("shot_id"),
                region.get("scene_id"),
            )
            for region in fixture.text.regions
        ],
    )
    connection.executemany(
        "INSERT INTO text_observations(text_observation_id, text_region_id, raw_text, "
        "normalized_text, layout_class) VALUES (?, ?, ?, ?, ?)",
        [
            (
                observation["text_observation_id"],
                observation["text_region_id"],
                observation["raw_text"],
                observation["normalized_text"],
                observation.get("layout_class"),
            )
            for observation in fixture.text.observations
        ],
    )
    connection.executemany(
        "INSERT INTO numeric_values(numeric_value_id, text_observation_id, text_region_id, "
        "numeric_value, raw_text, normalized_text) VALUES (?, ?, ?, ?, ?, ?)",
        [
            (
                numeric["numeric_value_id"],
                numeric["text_observation_id"],
                numeric["text_region_id"],
                numeric["numeric_value"],
                numeric["raw_text"],
                numeric["normalized_text"],
            )
            for numeric in fixture.text.numeric_values
        ],
    )
    connection.executemany(
        "INSERT INTO text_fts(object_id, object_type, start_us, end_us, text) "
        "VALUES (?, ?, ?, ?, ?)",
        [
            (
                observation["text_observation_id"],
                "text_observation",
                0,
                1000000,
                observation["normalized_text"],
            )
            for observation in fixture.text.observations
        ],
    )

    connection.executemany(
        "INSERT INTO color_observations(color_observation_id, target_type, target_id, "
        "dominant_bucket) VALUES (?, ?, ?, ?)",
        [
            (
                color["color_observation_id"],
                color["target_type"],
                color["target_id"],
                color["dominant_bucket"],
            )
            for color in fixture.colors.observations
        ],
    )
    connection.executemany(
        "INSERT INTO color_targets(color_observation_id, target_type, target_id) "
        "VALUES (?, ?, ?)",
        [
            (color["color_observation_id"], color["target_type"], color["target_id"])
            for color in fixture.colors.observations
        ],
    )
    connection.executemany(
        "INSERT INTO color_bucket_coverage(color_observation_id, bucket_id, coverage) "
        "VALUES (?, ?, ?)",
        [
            (color["color_observation_id"], bucket_id, amount)
            for color in fixture.colors.observations
            for bucket_id, amount in color["bucket_coverage"].items()
        ],
    )


def create_index_bytes(fixture: FixtureCase) -> tuple[bytes, int, int]:
    with tempfile.TemporaryDirectory(prefix="svp-phase04-index-") as temp_dir:
        sqlite_path = Path(temp_dir) / "index.sqlite"
        connection = sqlite3.connect(sqlite_path)
        try:
            for statement in INDEX_TABLES:
                connection.execute(statement)
            insert_index_rows(connection, fixture)
            connection.commit()
            table_count = connection.execute(
                "SELECT count(*) FROM sqlite_schema "
                "WHERE type = 'table' AND name NOT LIKE 'sqlite_%'"
            ).fetchone()[0]
            table_names = [
                row[0]
                for row in connection.execute(
                    "SELECT name FROM sqlite_schema "
                    "WHERE type = 'table' AND name NOT LIKE 'sqlite_%'"
                )
            ]
            row_count = sum(
                connection.execute(f'SELECT count(*) FROM "{table}"').fetchone()[0]
                for table in table_names
            )
        finally:
            connection.close()
        return sqlite_path.read_bytes(), table_count, row_count


def index_manifest(table_count: int, row_count: int) -> dict[str, Any]:
    return {
        "created_from": {
            "binary_blocks_manifest_blake3": HASH_THREE,
            "embedding_sets_blake3": HASH_FOUR,
            "manifest_blake3": HASH_TWO,
        },
        "index_schema_version": "svp-index-v1",
        "logical_row_stream_version": "svp-logical-row-stream-v1",
        "logical_rows_blake3": HASH_ONE,
        "row_count": row_count,
        "schema_version": "svp-index-manifest-v1",
        "sqlite_file": "index/index.sqlite",
        "sqlite_file_blake3": HASH_ZERO,
        "table_count": table_count,
    }


def base_entries(fixture: FixtureCase) -> dict[str, bytes]:
    colors = fixture.colors
    text = fixture.text
    return {
        "manifest.json": json_bytes(manifest(fixture.package_id)),
        "media/original/source_000.txt": b"SVP Phase 04 synthetic fixture source.\n",
        "media/audio/original_stream_000.flac": b"",
        "media/audio/analysis_mono_16k.wav": b"",
        "media/audio/waveform.jsonl": b"",
        "media/audio/audio_absence.json": json_bytes(
            {
                "source_audio_present": False,
                "reason": "synthetic_silent_fixture",
                "provenance_id": "processor_audio_fixture_0001",
            }
        ),
        "transcript/speech_regions.jsonl": b"",
        "transcript/transcript.json": json_bytes({"word_count": 0}),
        "transcript/words.jsonl": b"",
        "transcript/speakers.jsonl": b"",
        "transcript/speaker_segments.jsonl": b"",
        "timeline/frames.jsonl": jsonl_bytes([frame_record()]),
        "timeline/shots.jsonl": jsonl_bytes([shot_record()]),
        "timeline/scenes.jsonl": jsonl_bytes([scene_record()]),
        "entities/entities.jsonl": b"",
        "entities/entity_tracks.jsonl": b"",
        "spatial/regions.jsonl": b"",
        "spatial/masks.index.jsonl": b"",
        "spatial/masks.blocks.svpmz": b"",
        "spatial/depth.index.jsonl": b"",
        "spatial/depth.blocks.svpdz": b"",
        "text/text_regions.jsonl": jsonl_bytes(text.regions),
        "text/text_observations.jsonl": jsonl_bytes(text.observations),
        "text/numeric_values.jsonl": jsonl_bytes(text.numeric_values),
        "text/text_absence.json": json_bytes(text_absence(text)),
        "colors/color_observations.jsonl": jsonl_bytes(colors.observations),
        "colors/color_summary.json": json_bytes(color_summary(colors)),
        "colors/color_absence.json": json_bytes(color_absence(colors)),
        "relationships/relationships.jsonl": b"",
        "embeddings/embedding_sets.json": json_bytes({"embedding_sets": []}),
        "embeddings/embeddings.index.jsonl": b"",
        "embeddings/embeddings.blocks.svpez": b"",
        "provenance/build.json": json_bytes(
            {
                "builder": "phase04-fixture-generator",
                "created_utc": "2026-06-19T00:00:00Z",
            }
        ),
        "provenance/processors.jsonl": jsonl_bytes(processor_records()),
        "provenance/input_hashes.jsonl": b"",
        "provenance/model_hashes.jsonl": b"",
        "provenance/validation.json": json_bytes({"status": "fixture_unvalidated"}),
    }


def zip_info(path: str, compression: int) -> zipfile.ZipInfo:
    info = zipfile.ZipInfo(path, FIXED_ZIP_TIME)
    info.compress_type = compression
    info.external_attr = 0o644 << 16
    return info


def write_package(output_path: Path, fixture: FixtureCase) -> None:
    sqlite_bytes, table_count, row_count = create_index_bytes(fixture)
    entries = base_entries(fixture)
    entries["index/index.sqlite"] = sqlite_bytes
    entries["index/index_manifest.json"] = json_bytes(index_manifest(table_count, row_count))

    output_path.parent.mkdir(parents=True, exist_ok=True)
    if output_path.exists():
        output_path.unlink()

    directories = sorted({entry.rsplit("/", 1)[0] + "/" for entry in entries if "/" in entry})
    with zipfile.ZipFile(output_path, "w", allowZip64=True) as package:
        package.writestr(zip_info("mimetype", zipfile.ZIP_STORED), MIMETYPE)
        for directory in directories:
            dir_info = zipfile.ZipInfo(directory, FIXED_ZIP_TIME)
            dir_info.compress_type = zipfile.ZIP_STORED
            dir_info.external_attr = 0o755 << 16
            package.writestr(dir_info, b"")
        for path in sorted(entries):
            compression = zipfile.ZIP_STORED if path == "index/index.sqlite" else zipfile.ZIP_DEFLATED
            package.writestr(zip_info(path, compression), entries[path])


def fixture_cases() -> list[FixtureCase]:
    ui_colors = baseline_color_observations("gray") + [
        color_observation(
            "color_obs_000003",
            "text_region",
            "text_region_000001",
            {"red": 0.72, "black": 0.18, "other": 0.1},
            "red",
            "text_foreground",
        ),
        color_observation(
            "color_obs_000004",
            "text_region",
            "text_region_000001",
            {"white": 0.9, "gray": 0.1},
            "white",
            "text_background",
        ),
    ]
    ui_region = text_region("text_region_000001", "color_obs_000003", "color_obs_000004")

    numeric_colors = baseline_color_observations("gray") + [
        color_observation(
            "color_obs_000003",
            "text_region",
            "text_region_000001",
            {"green": 0.8, "black": 0.1, "other": 0.1},
            "green",
            "text_foreground",
        )
    ]
    numeric_region = text_region("text_region_000001", "color_obs_000003")

    invalid_reference_text = TextCase(
        regions=[text_region("text_region_000001")],
        observations=[
            text_observation(
                "Broken Ref",
                "broken ref",
                region_id="text_region_999999",
            )
        ],
        absence_reason="text_detected",
    )

    invalid_total_colors = [
        color_observation(
            "color_obs_000001",
            "scene",
            "scene_000001",
            {"orange": 0.6, "black": 0.2, "white": 0.1},
            "orange",
            "full_frame",
        )
    ]

    invalid_bucket_colors = [
        color_observation(
            "color_obs_000001",
            "shot",
            "shot_000001",
            {"ultraviolet": 0.5, "other": 0.5},
            "ultraviolet",
            "keyframe_full_frame",
        )
    ]

    return [
        FixtureCase(
            "valid/text_absence_no_visible_text.svp",
            "svp_fixture_text_absence_no_visible_text",
            TextCase(),
            ColorCase(baseline_color_observations("gray"), "color_observed"),
        ),
        FixtureCase(
            "valid/visible_ui_text.svp",
            "svp_fixture_visible_ui_text",
            TextCase(
                regions=[ui_region],
                observations=[text_observation("Tap Start", "tap start")],
                absence_reason="text_detected",
            ),
            ColorCase(ui_colors, "color_observed"),
        ),
        FixtureCase(
            "valid/numeric_text_price.svp",
            "svp_fixture_numeric_text_price",
            TextCase(
                regions=[numeric_region],
                observations=[text_observation("$12.99", "12.99")],
                numeric_values=[
                    numeric_value("$12.99", "12.99", "12.99", number_kind="currency")
                ],
                absence_reason="text_detected",
            ),
            ColorCase(numeric_colors, "color_observed"),
        ),
        FixtureCase(
            "valid/orange_scene_color.svp",
            "svp_fixture_orange_scene_color",
            TextCase(),
            ColorCase(
                [
                    color_observation(
                        "color_obs_000001",
                        "scene",
                        "scene_000001",
                        coverage("orange", 0.7),
                        "orange",
                        "full_frame",
                    ),
                    color_observation(
                        "color_obs_000002",
                        "shot",
                        "shot_000001",
                        coverage("orange", 0.7),
                        "orange",
                        "keyframe_full_frame",
                    ),
                ],
                "color_observed",
            ),
        ),
        FixtureCase(
            "valid/yellow_shot_color.svp",
            "svp_fixture_yellow_shot_color",
            TextCase(),
            ColorCase(
                [
                    color_observation(
                        "color_obs_000001",
                        "scene",
                        "scene_000001",
                        coverage("gray", 0.82),
                        "gray",
                        "full_frame",
                    ),
                    color_observation(
                        "color_obs_000002",
                        "shot",
                        "shot_000001",
                        coverage("yellow", 0.64),
                        "yellow",
                        "keyframe_full_frame",
                    ),
                ],
                "color_observed",
            ),
        ),
        FixtureCase(
            "valid/uniform_black_frame.svp",
            "svp_fixture_uniform_black_frame",
            TextCase(),
            ColorCase(
                baseline_color_observations("black")
                + [
                    color_observation(
                        "color_obs_000003",
                        "frame",
                        "frame_000001",
                        {"black": 1.0},
                        "black",
                        "full_frame",
                    )
                ],
                "uniform_color_observed",
            ),
        ),
        FixtureCase(
            "valid/uniform_white_frame.svp",
            "svp_fixture_uniform_white_frame",
            TextCase(),
            ColorCase(
                baseline_color_observations("white")
                + [
                    color_observation(
                        "color_obs_000003",
                        "frame",
                        "frame_000001",
                        {"white": 1.0},
                        "white",
                        "full_frame",
                    )
                ],
                "uniform_color_observed",
            ),
        ),
        FixtureCase(
            "valid/color_absence_no_observations.svp",
            "svp_fixture_color_absence_no_observations",
            TextCase(),
            ColorCase([], "processor_completed"),
        ),
        FixtureCase(
            "invalid/invalid_color_totals.svp",
            "svp_fixture_invalid_color_totals",
            TextCase(),
            ColorCase(invalid_total_colors, "color_observed"),
        ),
        FixtureCase(
            "invalid/invalid_color_bucket_id.svp",
            "svp_fixture_invalid_color_bucket_id",
            TextCase(),
            ColorCase(invalid_bucket_colors, "color_observed"),
        ),
        FixtureCase(
            "invalid/invalid_text_reference.svp",
            "svp_fixture_invalid_text_reference",
            invalid_reference_text,
            ColorCase(baseline_color_observations("gray"), "color_observed"),
        ),
    ]


def generate(output_dir: Path) -> list[Path]:
    written: list[Path] = []
    for fixture in fixture_cases():
        output_path = output_dir / fixture.name
        write_package(output_path, fixture)
        written.append(output_path)
    return written


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--output-dir",
        type=Path,
        default=DEFAULT_OUTPUT_DIR,
        help="Directory where fixture .svp files are written.",
    )
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    output_dir = args.output_dir.resolve()
    written = generate(output_dir)
    for path in written:
        print(path.relative_to(REPO_ROOT))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
