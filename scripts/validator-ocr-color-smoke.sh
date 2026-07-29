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
import ctypes
import ctypes.util
import copy
import json
import os
import pathlib
import sqlite3
import struct
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
UINT64_MAX = (1 << 64) - 1
BLAKE3_OUT_LEN = 32
BLAKE3_BLOCK_LEN = 64
BLAKE3_MAX_DEPTH = 54

def load_native_library(name):
    configured_dir = os.environ.get("SVP_NATIVE_LIBRARY_DIR")
    if configured_dir:
        directory = pathlib.Path(configured_dir)
        for filename in (f"lib{name}.dylib", f"lib{name}.so"):
            candidate = directory / filename
            if candidate.exists():
                return ctypes.CDLL(str(candidate))
        raise SystemExit(
            f"Error: SVP_NATIVE_LIBRARY_DIR does not contain lib{name}.dylib or lib{name}.so"
        )

    path = ctypes.util.find_library(name)
    if path is None:
        fallback = pathlib.Path("/opt/homebrew/lib") / f"lib{name}.dylib"
        if fallback.exists():
            path = str(fallback)
    if path is None:
        raise SystemExit(
            f"Error: could not find native library {name}; set SVP_NATIVE_LIBRARY_DIR"
        )
    return ctypes.CDLL(path)

libblake3 = load_native_library("blake3")
libzstd = load_native_library("zstd")

class Blake3ChunkState(ctypes.Structure):
    _fields_ = [
        ("cv", ctypes.c_uint32 * 8),
        ("chunk_counter", ctypes.c_uint64),
        ("buf", ctypes.c_uint8 * BLAKE3_BLOCK_LEN),
        ("buf_len", ctypes.c_uint8),
        ("blocks_compressed", ctypes.c_uint8),
        ("flags", ctypes.c_uint8),
    ]

class Blake3Hasher(ctypes.Structure):
    _fields_ = [
        ("key", ctypes.c_uint32 * 8),
        ("chunk", Blake3ChunkState),
        ("cv_stack_len", ctypes.c_uint8),
        ("cv_stack", ctypes.c_uint8 * ((BLAKE3_MAX_DEPTH + 1) * BLAKE3_OUT_LEN)),
    ]

libblake3.blake3_hasher_init.argtypes = [ctypes.POINTER(Blake3Hasher)]
libblake3.blake3_hasher_update.argtypes = [
    ctypes.POINTER(Blake3Hasher),
    ctypes.c_void_p,
    ctypes.c_size_t,
]
libblake3.blake3_hasher_finalize.argtypes = [
    ctypes.POINTER(Blake3Hasher),
    ctypes.POINTER(ctypes.c_uint8),
    ctypes.c_size_t,
]
libzstd.ZSTD_compressBound.argtypes = [ctypes.c_size_t]
libzstd.ZSTD_compressBound.restype = ctypes.c_size_t
libzstd.ZSTD_compress.argtypes = [
    ctypes.c_void_p,
    ctypes.c_size_t,
    ctypes.c_void_p,
    ctypes.c_size_t,
    ctypes.c_int,
]
libzstd.ZSTD_compress.restype = ctypes.c_size_t
libzstd.ZSTD_isError.argtypes = [ctypes.c_size_t]
libzstd.ZSTD_isError.restype = ctypes.c_uint
libzstd.ZSTD_getErrorName.argtypes = [ctypes.c_size_t]
libzstd.ZSTD_getErrorName.restype = ctypes.c_char_p

def blake3_digest(data):
    hasher = Blake3Hasher()
    libblake3.blake3_hasher_init(ctypes.byref(hasher))
    if data:
        source = ctypes.create_string_buffer(data)
        libblake3.blake3_hasher_update(ctypes.byref(hasher), source, len(data))
    digest = (ctypes.c_uint8 * BLAKE3_OUT_LEN)()
    libblake3.blake3_hasher_finalize(ctypes.byref(hasher), digest, BLAKE3_OUT_LEN)
    return bytes(digest)

def zstd_compress(data):
    bound = libzstd.ZSTD_compressBound(len(data))
    output = ctypes.create_string_buffer(bound)
    source = ctypes.create_string_buffer(data)
    size = libzstd.ZSTD_compress(output, bound, source, len(data), 1)
    if libzstd.ZSTD_isError(size):
        raise RuntimeError(libzstd.ZSTD_getErrorName(size).decode("utf-8"))
    return output.raw[:size]

def blake3_hex(data):
    return "blake3:" + blake3_digest(data).hex()

def sqlite_identifier(name):
    return '"' + name.replace('"', '""') + '"'

def normalized_sql(value):
    return " ".join(value.split())

def canonical_sqlite_value(value):
    if value is None:
        return ["null"]
    if isinstance(value, int):
        return ["integer", value]
    if isinstance(value, float):
        return ["real", value]
    if isinstance(value, bytes):
        return ["blob", value.hex()]
    return ["text", value.encode("utf-8").hex()]

def canonical_json_bytes(value):
    return json.dumps(value, separators=(",", ":"), sort_keys=True).encode("utf-8")

def canonical_json_line(value):
    return json.dumps(value, separators=(",", ":")).encode("utf-8") + b"\n"

def user_table_names(connection):
    try:
        rows = connection.execute("PRAGMA table_list").fetchall()
    except sqlite3.DatabaseError:
        rows = []
    if rows:
        return sorted(
            row[1]
            for row in rows
            if row[0] == "main" and row[2] in {"table", "virtual"} and not row[1].startswith("sqlite_")
        )

    return [
        row[0]
        for row in connection.execute(
            "SELECT name FROM sqlite_schema WHERE type = 'table' AND name NOT LIKE 'sqlite_%' ORDER BY name"
        )
    ]

def table_columns(connection, table_name):
    return [
        {"name": row[1], "primary_key_order": row[5]}
        for row in connection.execute(f"PRAGMA table_info({sqlite_identifier(table_name)})")
    ]

def schema_fingerprint(connection, table_name):
    sql_row = connection.execute(
        "SELECT sql FROM sqlite_schema WHERE name = ? AND type = 'table'",
        (table_name,),
    ).fetchone()
    source = {
        "table": table_name,
        "sql": normalized_sql(sql_row[0] if sql_row and sql_row[0] else ""),
        "columns": [],
    }
    for row in connection.execute(f"PRAGMA table_xinfo({sqlite_identifier(table_name)})"):
        source["columns"].append(
            {
                "cid": row[0],
                "name": row[1] or "",
                "type": row[2] or "",
                "notnull": row[3],
                "default": row[4],
                "pk": row[5],
                "hidden": row[6],
            }
        )
    return blake3_hex(canonical_json_bytes(source))

def order_expression(column_name):
    column = sqlite_identifier(column_name)
    return (
        f"CASE typeof({column}) WHEN 'null' THEN 0 WHEN 'integer' THEN 1 WHEN 'real' THEN 2 "
        f"WHEN 'text' THEN 3 WHEN 'blob' THEN 4 ELSE 5 END, "
        f"CASE WHEN typeof({column}) IN ('integer', 'real') THEN {column} END, "
        f"CASE WHEN typeof({column}) = 'text' THEN {column} END COLLATE BINARY, "
        f"CASE WHEN typeof({column}) = 'blob' THEN hex({column}) END"
    )

def select_rows_query(table_name, columns):
    select_columns = ", ".join(sqlite_identifier(column["name"]) for column in columns)
    query = f"SELECT {select_columns} FROM {sqlite_identifier(table_name)}"
    primary_key_columns = sorted(
        [column for column in columns if column["primary_key_order"] > 0],
        key=lambda column: column["primary_key_order"],
    )
    order_columns = primary_key_columns or columns
    if order_columns:
        query += " ORDER BY " + ", ".join(order_expression(column["name"]) for column in order_columns)
    return query

def logical_row_stream_summary(connection):
    stream = bytearray()
    row_count = 0
    tables = user_table_names(connection)
    for table_name in tables:
        columns = table_columns(connection, table_name)
        column_names = [column["name"] for column in columns]
        stream.extend(canonical_json_line(["table", table_name]))
        stream.extend(canonical_json_line(["schema", table_name, schema_fingerprint(connection, table_name)]))
        stream.extend(canonical_json_line(["columns", table_name, column_names]))
        if not columns:
            continue
        for row in connection.execute(select_rows_query(table_name, columns)):
            stream.extend(
                canonical_json_line(
                    ["row", table_name, [canonical_sqlite_value(value) for value in row]]
                )
            )
            row_count += 1
    return {
        "logical_rows_blake3": blake3_hex(bytes(stream)),
        "table_count": len(tables),
        "row_count": row_count,
    }

def block_header(
    *,
    magic=b"SVPB",
    block_type,
    uncompressed_size,
    compressed_size,
    extent_0,
    extent_1,
    extent_2,
    dtype,
    start_frame,
    frame_count,
    start_us,
    end_us,
    payload_blake3,
    header_blake3=None,
):
    if header_blake3 is None:
        header_blake3 = b"\x00" * 32
    return struct.pack(
        "<4sHHBBBBQQIIIIQQqq32s32s20s",
        magic,
        1,
        160,
        block_type,
        1,
        1,
        0,
        uncompressed_size,
        compressed_size,
        extent_0,
        extent_1,
        extent_2,
        dtype,
        start_frame,
        frame_count,
        start_us,
        end_us,
        payload_blake3,
        header_blake3,
        b"\x00" * 20,
    )

def svpb_block(
    *,
    magic=b"SVPB",
    block_type,
    payload,
    compressed_payload=None,
    payload_blake3=None,
    extent_0,
    extent_1,
    extent_2,
    dtype,
    start_frame,
    frame_count,
    start_us,
    end_us,
):
    if compressed_payload is None:
        compressed_payload = zstd_compress(payload)
    if payload_blake3 is None:
        payload_blake3 = blake3_digest(compressed_payload)
    header_without_header_hash = block_header(
        magic=magic,
        block_type=block_type,
        uncompressed_size=len(payload),
        compressed_size=len(compressed_payload),
        extent_0=extent_0,
        extent_1=extent_1,
        extent_2=extent_2,
        dtype=dtype,
        start_frame=start_frame,
        frame_count=frame_count,
        start_us=start_us,
        end_us=end_us,
        payload_blake3=payload_blake3,
    )
    header_blake3 = blake3_digest(header_without_header_hash)
    return block_header(
        magic=magic,
        block_type=block_type,
        uncompressed_size=len(payload),
        compressed_size=len(compressed_payload),
        extent_0=extent_0,
        extent_1=extent_1,
        extent_2=extent_2,
        dtype=dtype,
        start_frame=start_frame,
        frame_count=frame_count,
        start_us=start_us,
        end_us=end_us,
        payload_blake3=payload_blake3,
        header_blake3=header_blake3,
    ) + compressed_payload

def tiny_depth_block(
    *,
    magic=b"SVPB",
    block_type=1,
    width=640,
    payload_blake3=None,
    compressed_payload=None,
):
    payload = b"\x00" * (width * 360 * 2)
    return svpb_block(
        magic=magic,
        block_type=block_type,
        payload=payload,
        compressed_payload=compressed_payload,
        payload_blake3=payload_blake3,
        extent_0=width,
        extent_1=360,
        extent_2=1,
        dtype=2,
        start_frame=0,
        frame_count=1,
        start_us=0,
        end_us=33333,
    )

def tiny_embedding_block():
    return svpb_block(
        block_type=3,
        payload=b"\x00\x00\x00\x00",
        extent_0=1,
        extent_1=1,
        extent_2=1,
        dtype=4,
        start_frame=UINT64_MAX,
        frame_count=0,
        start_us=-1,
        end_us=-1,
    )

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

def index_manifest(index_summary):
    return {
        "schema_version": "svp-index-manifest-v1",
        "index_schema_version": "svp-index-v1",
        "sqlite_file": "index/index.sqlite",
        "sqlite_file_blake3": HASH_ZERO,
        "logical_row_stream_version": "svp-logical-row-stream-v1",
        "logical_rows_blake3": index_summary["logical_rows_blake3"],
        "table_count": index_summary["table_count"],
        "row_count": index_summary["row_count"],
        "created_from": {
            "manifest_blake3": HASH_TWO,
            "binary_blocks_manifest_blake3": HASH_THREE,
            "embedding_sets_blake3": HASH_FOUR,
        },
    }

def create_index_bytes(missing_table=None, scalar_case=None):
    sqlite_path = root / f"index-{missing_table or 'valid'}.sqlite"
    if sqlite_path.exists():
        sqlite_path.unlink()

    connection = sqlite3.connect(sqlite_path)
    try:
        for statement in INDEX_TABLES:
            if missing_table and f"CREATE TABLE {missing_table} " in statement:
                continue
            connection.execute(statement)
        if scalar_case in {"blob_embedding", "blob_manifest_text_database"}:
            connection.execute(
                """
                INSERT INTO vector_index (
                  embedding,
                  object_id,
                  object_type,
                  embedding_set_id,
                  start_us,
                  end_us
                ) VALUES (?, ?, ?, ?, ?, ?)
                """,
                (
                    sqlite3.Binary(b"\xff"),
                    "object_scalar_probe",
                    "probe",
                    "embedding_set_probe",
                    0,
                    1,
                ),
            )
        connection.commit()
        index_summary = logical_row_stream_summary(connection)
        if scalar_case == "blob_manifest_text_database":
            connection.execute(
                "UPDATE vector_index SET embedding = ? WHERE object_id = ?",
                ("blob:ff", "object_scalar_probe"),
            )
            connection.commit()
    finally:
        connection.close()

    data = sqlite_path.read_bytes()
    sqlite_path.unlink()
    return data, index_summary

def write_package(
    name,
    mutate,
    text_regions_payload=None,
    index_case="valid",
    depth_block_case="valid",
    include_embedding_entries=True,
):
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
        package.writestr("spatial/depth.index.jsonl", "", compress_type=zipfile.ZIP_DEFLATED)
        depth_block = tiny_depth_block()
        depth_compress_type = zipfile.ZIP_STORED
        if depth_block_case == "bad_magic":
            depth_block = tiny_depth_block(magic=b"NOPE")
        elif depth_block_case == "forbidden_type":
            depth_block = tiny_depth_block(block_type=4)
        elif depth_block_case == "raster_mismatch":
            depth_block = tiny_depth_block(width=320)
        elif depth_block_case == "zip_deflated":
            depth_compress_type = zipfile.ZIP_DEFLATED
        elif depth_block_case == "bad_payload_hash":
            depth_block = tiny_depth_block(payload_blake3=b"\xff" * 32)
        elif depth_block_case == "bad_zstd_payload":
            depth_block = tiny_depth_block(compressed_payload=b"not-zstd")
        package.writestr("spatial/depth.blocks.svpdz", depth_block, compress_type=depth_compress_type)
        package.writestr("spatial/masks.index.jsonl", "", compress_type=zipfile.ZIP_DEFLATED)
        package.writestr("spatial/masks.blocks.svpmz", b"", compress_type=zipfile.ZIP_STORED)
        if include_embedding_entries:
            package.writestr("embeddings/embedding_sets.json", "{\"sets\":[]}", compress_type=zipfile.ZIP_DEFLATED)
            package.writestr("embeddings/embeddings.index.jsonl", "", compress_type=zipfile.ZIP_DEFLATED)
            package.writestr("embeddings/embeddings.blocks.svpez", tiny_embedding_block(), compress_type=zipfile.ZIP_STORED)
        if index_case == "missing_manifest":
            sqlite_bytes, index_summary = create_index_bytes()
            package.writestr("index/index.sqlite", sqlite_bytes)
        elif index_case == "malformed_manifest":
            sqlite_bytes, index_summary = create_index_bytes()
            package.writestr("index/index.sqlite", sqlite_bytes)
            package.writestr("index/index_manifest.json", "{not json")
        elif index_case == "empty_index_schema_version":
            sqlite_bytes, index_summary = create_index_bytes()
            manifest_value = index_manifest(index_summary)
            manifest_value["index_schema_version"] = ""
            package.writestr("index/index.sqlite", sqlite_bytes)
            package.writestr("index/index_manifest.json", json.dumps(manifest_value, separators=(",", ":")))
        elif index_case == "missing_sqlite":
            _, index_summary = create_index_bytes()
            package.writestr("index/index_manifest.json", json.dumps(index_manifest(index_summary), separators=(",", ":")))
        elif index_case == "unreadable_sqlite":
            package.writestr("index/index.sqlite", b"not sqlite")
            package.writestr("index/index_manifest.json", json.dumps(index_manifest({
                "logical_rows_blake3": HASH_ONE,
                "table_count": 0,
                "row_count": 0,
            }), separators=(",", ":")))
        elif index_case == "missing_color_table":
            sqlite_bytes, index_summary = create_index_bytes("color_bucket_coverage")
            package.writestr("index/index.sqlite", sqlite_bytes)
            package.writestr("index/index_manifest.json", json.dumps(index_manifest(index_summary), separators=(",", ":")))
        elif index_case == "logical_hash_mismatch":
            sqlite_bytes, index_summary = create_index_bytes()
            manifest_value = index_manifest(index_summary)
            manifest_value["logical_rows_blake3"] = HASH_ONE
            package.writestr("index/index.sqlite", sqlite_bytes)
            package.writestr("index/index_manifest.json", json.dumps(manifest_value, separators=(",", ":")))
        elif index_case == "row_count_mismatch":
            sqlite_bytes, index_summary = create_index_bytes()
            manifest_value = index_manifest(index_summary)
            manifest_value["row_count"] = index_summary["row_count"] + 1
            package.writestr("index/index.sqlite", sqlite_bytes)
            package.writestr("index/index_manifest.json", json.dumps(manifest_value, separators=(",", ":")))
        elif index_case == "typed_value_mismatch":
            sqlite_bytes, index_summary = create_index_bytes(scalar_case="blob_manifest_text_database")
            package.writestr("index/index.sqlite", sqlite_bytes)
            package.writestr("index/index_manifest.json", json.dumps(index_manifest(index_summary), separators=(",", ":")))
        else:
            sqlite_bytes, index_summary = create_index_bytes()
            package.writestr("index/index.sqlite", sqlite_bytes)
            package.writestr("index/index_manifest.json", json.dumps(index_manifest(index_summary), separators=(",", ":")))

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
write_package("logical-row-hash-mismatch", no_change, index_case="logical_hash_mismatch")
write_package("logical-row-count-mismatch", no_change, index_case="row_count_mismatch")
write_package("logical-row-typed-value-mismatch", no_change, index_case="typed_value_mismatch")
write_package("invalid-svpb-magic", no_change, depth_block_case="bad_magic")
write_package("forbidden-svpb-type", no_change, depth_block_case="forbidden_type")
write_package("svpb-raster-mismatch", no_change, depth_block_case="raster_mismatch")
write_package("deflated-svpb-entry", no_change, depth_block_case="zip_deflated")
write_package("svpb-payload-hash-mismatch", no_change, depth_block_case="bad_payload_hash")
write_package("svpb-payload-decode-failed", no_change, depth_block_case="bad_zstd_payload")
write_package("missing-embedding-entries", no_change, include_embedding_entries=False)
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

logical_hash_mismatch_report="$workdir/logical-row-hash-mismatch.json"
logical_hash_mismatch_status="$(run_validator "$workdir/logical-row-hash-mismatch.svp" "$logical_hash_mismatch_report")"
expect_status "$logical_hash_mismatch_status" "1" "logical row hash mismatch package"
expect_code "$logical_hash_mismatch_report" "ERR_CORE_INDEX_LOGICAL_MISMATCH"

logical_row_count_mismatch_report="$workdir/logical-row-count-mismatch.json"
logical_row_count_mismatch_status="$(run_validator "$workdir/logical-row-count-mismatch.svp" "$logical_row_count_mismatch_report")"
expect_status "$logical_row_count_mismatch_status" "1" "logical row count mismatch package"
expect_code "$logical_row_count_mismatch_report" "ERR_CORE_INDEX_LOGICAL_MISMATCH"

logical_typed_value_mismatch_report="$workdir/logical-row-typed-value-mismatch.json"
logical_typed_value_mismatch_status="$(run_validator "$workdir/logical-row-typed-value-mismatch.svp" "$logical_typed_value_mismatch_report")"
expect_status "$logical_typed_value_mismatch_status" "1" "logical typed value mismatch package"
expect_code "$logical_typed_value_mismatch_report" "ERR_CORE_INDEX_LOGICAL_MISMATCH"

invalid_svpb_magic_report="$workdir/invalid-svpb-magic.json"
invalid_svpb_magic_status="$(run_validator "$workdir/invalid-svpb-magic.svp" "$invalid_svpb_magic_report")"
expect_status "$invalid_svpb_magic_status" "1" "invalid SVPB magic package"
expect_code "$invalid_svpb_magic_report" "ERR_CORE_INVALID_BLOCK_HEADER"

forbidden_svpb_type_report="$workdir/forbidden-svpb-type.json"
forbidden_svpb_type_status="$(run_validator "$workdir/forbidden-svpb-type.svp" "$forbidden_svpb_type_report")"
expect_status "$forbidden_svpb_type_status" "1" "forbidden SVPB block type package"
expect_code "$forbidden_svpb_type_report" "ERR_CORE_FORBIDDEN_BLOCK_TYPE"

svpb_raster_mismatch_report="$workdir/svpb-raster-mismatch.json"
svpb_raster_mismatch_status="$(run_validator "$workdir/svpb-raster-mismatch.svp" "$svpb_raster_mismatch_report")"
expect_status "$svpb_raster_mismatch_status" "1" "SVPB raster mismatch package"
expect_code "$svpb_raster_mismatch_report" "ERR_CORE_RASTER_EXTENT_MISMATCH"

deflated_svpb_entry_report="$workdir/deflated-svpb-entry.json"
deflated_svpb_entry_status="$(run_validator "$workdir/deflated-svpb-entry.svp" "$deflated_svpb_entry_report")"
expect_status "$deflated_svpb_entry_status" "1" "deflated SVPB entry package"
expect_code "$deflated_svpb_entry_report" "X_VALIDATOR_BLOCK_ENTRY_NOT_STORED"

svpb_payload_hash_mismatch_report="$workdir/svpb-payload-hash-mismatch.json"
svpb_payload_hash_mismatch_status="$(run_validator "$workdir/svpb-payload-hash-mismatch.svp" "$svpb_payload_hash_mismatch_report")"
expect_status "$svpb_payload_hash_mismatch_status" "1" "SVPB payload hash mismatch package"
expect_code "$svpb_payload_hash_mismatch_report" "ERR_CORE_INVALID_BLOCK_HASH"

svpb_payload_decode_failed_report="$workdir/svpb-payload-decode-failed.json"
svpb_payload_decode_failed_status="$(run_validator "$workdir/svpb-payload-decode-failed.svp" "$svpb_payload_decode_failed_report")"
expect_status "$svpb_payload_decode_failed_status" "1" "SVPB payload decode failed package"
expect_code "$svpb_payload_decode_failed_report" "X_VALIDATOR_BLOCK_PAYLOAD_DECODE_FAILED"

missing_embedding_entries_report="$workdir/missing-embedding-entries.json"
missing_embedding_entries_status="$(run_validator "$workdir/missing-embedding-entries.svp" "$missing_embedding_entries_report")"
expect_status "$missing_embedding_entries_status" "1" "missing embedding entries package"
expect_code "$missing_embedding_entries_report" "X_VALIDATOR_MISSING_EMBEDDINGS_ENTRY"

echo "OCR/color and SVPB validator smoke checks passed."
