# SVP Package Export Format, Version 1

`svp-inspector export` turns an SVP package, an SVPI sidecar, or an Embedded
SVPI Transport into a plain directory that any downstream tool can read with an
ordinary JSON parser and a file reader. No ZIP, SQLite, SVPB block, or
Zstandard code is needed to consume an export.

The export is a faithful copy of what the package declares. It never
reinterprets, estimates, rounds, or repairs package content. Where it adds
information (resolved references, decoded binary payloads), the additions are
kept apart from the original data and are named as export additions.

Machine-readable contracts:

| Contract | Schema |
| --- | --- |
| `export.json` | [`spec/schemas/package-export.schema.json`](../../spec/schemas/package-export.schema.json) |
| Exported JSONL record and its `svp_export` member | [`spec/schemas/package-export-record.schema.json`](../../spec/schemas/package-export-record.schema.json) |
| Block table record (`*.blocks.jsonl`) | [`spec/schemas/package-export-block.schema.json`](../../spec/schemas/package-export-block.schema.json) |
| Command result printed on stdout | [`spec/schemas/package-export-result.schema.json`](../../spec/schemas/package-export-result.schema.json) |

## 1. Command

```bash
svp-inspector export <package> --out <dir> [--overwrite]
```

| Argument | Meaning |
| --- | --- |
| `<package>` | An `.svp` package, an `.svpi` sidecar, or an ISO BMFF file (`.mp4`, `.mov`, `.m4v`, `.m4a`) carrying an Embedded SVPI Transport. |
| `--out <dir>` | Directory that receives the export. It must not exist, or must be an empty directory. |
| `--overwrite` | Replace an existing `--out` that holds a previous export (a directory whose top level contains `export.json`). Any other non-empty directory, any non-directory, a symbolic link, and any directory that contains the input package are never replaced. |

The command prints exactly one JSON document on stdout, for success and for
every failure that happens after argument parsing (Section 9). It prints nothing
else on stdout.

### 1.1 Input detection

The input form is detected from content, not from the file name:

1. A file whose first top-level box structure is ISO BMFF (`ftyp` signature)
   is an Embedded SVPI Transport.
2. A ZIP archive whose `mimetype` entry is `application/vnd.svp+zip` is an SVP
   package.
3. A ZIP archive whose `mimetype` entry is
   `application/vnd.svp.interlace+zip` is an SVPI sidecar.
4. A ZIP archive with no recognized `mimetype` is treated by its extension
   (`.svp` or `.svpi`) so the validator can report what is wrong with it.
5. Anything else is refused as `input_unrecognized`.

### 1.2 Validation first

Before anything is written, the input is validated with the same validation
library calls `svp-validator validate` makes for that input form:

| Detected form | Validation call |
| --- | --- |
| SVP | `svp::validation::validate_package` |
| SVPI | `svp::validation::validate_svpi_package` |
| Embedded SVPI Transport | `svp::validation::validate_embedded_svpi_transport` |

The validators keep their own rules. In particular they still require the
`.svp` or `.svpi` extension for those forms, exactly as `svp-validator` does, so
a renamed package is refused the same way the validator refuses it.

A package whose validation status is `invalid` or `unreadable` is refused with
`package_invalid` and nothing is written. `valid` and `valid_with_warnings`
packages are exported; warning and info codes are summarized in `export.json`.

### 1.3 Supported versions

After validation, the declared format versions are checked against what this
build supports. The policy lives in
`tools/svp-inspector/src/export/version_policy.cpp`:

| Declared value | Where | Supported | Rule |
| --- | --- | --- | --- |
| `svp_version` | SVP `manifest.json` | `1.0-rc.2` | Required for SVP. |
| `svpi_version` | SVPI `manifest.json` | `0.1` (`svp::package::kSvpiVersion`) | Required for SVPI and embedded SVPI. |
| `svp_version` | SVPI `manifest.json` | `1.0-rc.2` | Checked when declared. |
| Transport profile version | Embedded SVPI envelope | `1` (`svp::package::kEmbeddedSvpiProfileVersion`) | Required for embedded SVPI. |

`1.0-rc.2` is the package format this build's validator implements and this
build's builder writes. A missing or different value is refused with
`unsupported_version`, even when validation passed, because this build cannot
promise that it understands that package's layers. A declared unsupported
version is reported in preference to validation findings, because the RC2
validator's findings may not apply to another version. An unsupported
transport profile version is also reported as `unsupported_version`, not as
`package_invalid`.

### 1.4 Atomic output

The export is written into a hidden staging directory next to `--out`
(`.<name>.svp-export-staging-XXXXXX`) and renamed onto `--out` only after every
file has been written and the input has been checked to be unchanged since
validation. Missing parent directories of `--out` are created once validation
and the output checks have passed. On any failure the staging directory is
removed and `--out` is left exactly as it was. With `--overwrite`, the previous
export is moved aside, the new export is renamed into place, and only then is
the previous export removed.

## 2. Export directory layout

```text
<out>/
  export.json                         summary (Section 3)
  layers/
    <entry path>                      one file per package entry, except block streams (Section 4)
    <block stream entry>.blocks.jsonl block table (Section 6)
    <block stream entry>.decoded.bin  decoded block payloads (Section 6)
```

Every package file entry appears under `layers/` at its own entry path, so a
path written inside a record (for example an evidence crop's
`crop_file_path: "text/evidence_crops/crop_000001.jpg"`) names
`layers/text/evidence_crops/crop_000001.jpg`. All paths that the export itself
writes (in `export.json` and in `svp_export` members) are relative to the
export root and use `/` separators.

ZIP directory entries (names ending in `/`) are not layers and produce no
files. For an Embedded SVPI Transport, only the embedded SVPI is exported; the
carrying media file is not copied.

## 3. `export.json`

`export.json` is UTF-8 JSON with object keys in sorted order and two-space
indentation. Its fields:

| Field | Meaning |
| --- | --- |
| `export_format` | Always `"svp-package-export"`. |
| `export_format_version` | Integer, `1` for this document. Readers must refuse a major version they do not know and must ignore unknown fields. |
| `exporter` | `{"tool": "svp-inspector", "tool_version": "<build version>"}`. |
| `package.kind` | `"svp"`, `"svpi"`, or `"embedded_svpi"`. |
| `package.mimetype` | Content of the package's `mimetype` entry, or `null`. |
| `package.format_versions` | `svp_version`, `svpi_version`, and `embedded_transport_profile_version` as declared (each `null` when not declared). |
| `package.embedded_transport` | For embedded SVPI: `container_kind`, `major_brand`, `profile_version`, `box_offset`, `box_size`, `payload_offset`, `payload_size`. Otherwise `null`. |
| `validation` | `validator` (`name`, `version`), `status`, `core_status`, and `errors` / `warnings` / `infos` as `[{"code", "count"}]` sorted by code. `errors` can only hold findings that do not affect core status, since an export only happens when validation passed. |
| `manifest` | The package `manifest.json` object, as declared. |
| `media_binding` | The `media_binding.json` object, as declared, or `null` when the package has none (SVP packages). This includes `bindings[].identity`, `streams`, `duration_us`, `verification_state`, and `location_hints` exactly as the package records them. |
| `sections` | One entry per section (Section 3.1). |
| `resolution_sources` | Which lookup layers were available for reference resolution (Section 5.1). |
| `layers` | One entry per exported package entry, sorted by entry name (Section 3.2). |

`export.json` never contains the input path, the output path, wall-clock time,
host names, or anything else that varies between runs.

### 3.1 Sections

`sections` lists the union of the top-level directories present in the package
and the section names the manifest declares, sorted by name:

| Field | Meaning |
| --- | --- |
| `name` | Section name, for example `transcript` or `text`. |
| `declared_state` | `manifest.sections.<name>.state` as declared (SVPI: for example `generated`, `present`, `declared_absent`, `not_generated`, `blocked`, `unsupported`, `omitted`), or `null`. |
| `declaration` | The whole `manifest.sections.<name>` value as declared, or `null`. |
| `declared_required` | `manifest.required_sections.<name>` as declared (SVP), or `null`. |
| `layer_count` | Number of entries in `layers` whose `section` is this name. |

The export reports declared states; it does not decide whether a declared state
is truthful. A section declared `generated` with empty record files is exported
as declared, with each layer's `state` set to `empty`.

### 3.2 Layers

| Field | Meaning |
| --- | --- |
| `entry` | Package entry name, for example `transcript/words.jsonl`. |
| `section` | Top-level directory of the entry, or `null` for root files such as `manifest.json`. |
| `representation` | `jsonl`, `json`, `block_stream`, or `file` (Section 4). |
| `files` | Files written for this entry: `[{"path", "role", "size_bytes"}]`. Roles: `records`, `document`, `bytes`, `block_table`, `decoded_payloads`. |
| `record_count` | `jsonl`: number of records. `block_stream`: number of blocks. Otherwise `null`. |
| `state` | `present` when the layer has at least one record or block (`jsonl`, `block_stream`), or is a non-empty file (`json`, `file`); otherwise `empty`. |
| `source_size_bytes` | Uncompressed size of the package entry. |
| `resolutions` | The reference rules applied to this layer: `[{"member", "kind"}]` (Section 5). Empty when none apply. |

## 4. Representations

The representation of an entry is chosen from its name, which the SVP and SVPI
layouts define:

| Representation | Entries | Export |
| --- | --- | --- |
| `jsonl` | names ending `.jsonl` | Records file, Section 4.1. |
| `json` | names ending `.json` | The document's bytes, unchanged. The export checks that it parses as JSON. |
| `block_stream` | names ending `.svpdz`, `.svpmz`, `.svpez` | Block table plus decoded payloads, Section 6. The compressed stream itself is not copied. |
| `file` | everything else (`mimetype`, evidence crop images, `index/index.sqlite`, packaged media in an SVP, extension files) | The entry's bytes, unchanged. |

### 4.1 JSONL records

Each exported JSONL file has one line per record, in package order, each line
ending in `\n`:

1. A record is a non-blank line of the package entry. Blank and
   whitespace-only lines are not records and are not copied.
2. The record must parse as a JSON object. The export does not check it
   against a layer schema; that is the validator's job.
3. The record's original JSON text is kept byte for byte, apart from removing
   leading and trailing whitespace (including a `\r` before `\n`). Field order,
   number spelling, string escapes, and every value are exactly as the package
   declares them. Times stay integer microseconds and IDs are unchanged.
4. When the layer has reference rules (Section 5) and the record holds at least
   one of the referencing members, the export appends one member, `svp_export`,
   at the end of the record. No original member is changed, removed, or
   reordered.

Removing `svp_export` from an exported record gives back the package record,
which then validates against its own layer schema (for example
`spec/schemas/text-observation.schema.json`).

`svp_export` is reserved. A package record that already has a member named
`svp_export` is refused with `reserved_member_present` rather than exported in
an ambiguous shape. No SVP RC2 or SVPI v0.1 record defines that member.

## 5. Reference resolution (`svp_export`)

Some records refer to other records by ID. To save every consumer from joining
the same files, the export adds the referenced values next to the original
IDs. `svp_export.<member>` always resolves the original member `<member>` of the
same record, and a list member resolves to a list in the same order, with
duplicates kept.

The rules are a fixed registry in
`tools/svp-inspector/src/export/reference_rules.cpp`:

| Layer entry | Member | Kind |
| --- | --- | --- |
| `text/text_observations.jsonl` | `source_frame_ids` | `frame_list` |
| `text/text_observations.jsonl` | `evidence_crop_refs` | `crop_list` |
| `text/evidence_crops.jsonl` | `source_frame_id` | `frame` |
| `text/evidence_crops.jsonl` | `crop_file_path` | `entry_path` |
| `colors/color_observations.jsonl` | `frame_ids` | `frame_list` |
| `timeline/shots.jsonl` | `start_frame_id`, `end_frame_id` | `frame` |
| `entities/entity_tracks.jsonl` | `start_frame_id`, `end_frame_id` | `frame` |
| `spatial/regions.jsonl` | `frame_id` | `frame` |
| `spatial/depth.index.jsonl` | `frame_id` | `frame` |
| `spatial/depth.index.jsonl` | `block_file` | `block` |
| `spatial/masks.index.jsonl` | `frame_id` | `frame` |
| `spatial/masks.index.jsonl` | `block_file` | `block` |
| `embeddings/embeddings.index.jsonl` | `block_file` | `block` |

A rule applies only when the member is present with the expected JSON type (a
string for `frame`, `entry_path`, and `block`; an array of strings for the
list kinds). A member with another type is left unresolved and gets no
`svp_export` entry. Resolution never fails an export: a reference that cannot
be resolved is reported with `"found": false`.

### 5.1 Lookup sources

| Kind | Looked up in | Key |
| --- | --- | --- |
| `frame`, `frame_list` | `timeline/frames.jsonl` | `id` |
| `crop`, `crop_list` | `text/evidence_crops.jsonl` | `crop_id` |
| `entry_path` | the package's own entry names | exact entry name |
| `block` | the decoded block table of the named block stream (Section 6) | `block_offset` |

If a key appears more than once in a lookup layer, the first record in package
order is used. `export.json` `resolution_sources` reports, for `frames` and
`evidence_crops`, the lookup `entry`, whether it is `present`, and its
`record_count`.

### 5.2 Resolved shapes

`frame` (and each element of `frame_list`) - the referenced frame's sample time,
copied from the frame record as declared:

```json
{"id": "frame_000016", "found": true, "frame_index": 15, "pts_us": 0}
{"id": "frame_999999", "found": false}
```

`frame_index` and `pts_us` are copied from the frame record; either is `null`
if that frame record does not declare it.

`crop` (and each element of `crop_list`) - the exported evidence image:

```json
{"id": "crop_000001", "found": true, "file": "layers/text/evidence_crops/crop_000001.jpg"}
```

`file` is `null` when the crop record has no string `crop_file_path`, or when
that path is not an entry the export mirrors under `layers/`.

`entry_path` - a package path written inside a record:

```json
{"path": "text/evidence_crops/crop_000001.jpg", "found": true, "file": "layers/text/evidence_crops/crop_000001.jpg"}
```

`block` - the decoded payload of the block an index record points at, matched
by the record's `block_file` and `block_offset`:

```json
{
  "block_file": "spatial/depth.blocks.svpdz",
  "block_offset": 0,
  "found": true,
  "block_ordinal": 0,
  "decoded_file": "layers/spatial/depth.blocks.svpdz.decoded.bin",
  "decoded_offset": 0,
  "decoded_length": 460800,
  "dtype": 2,
  "dtype_name": "uint16",
  "extents": [640, 360, 1]
}
```

`found` is `false` (and only `block_file` and `block_offset` are echoed) when
`block_offset` is not an unsigned integer, the stream is not a package entry, or
no block starts at that offset.

### 5.3 Example: a text observation

Package record (shortened):

```json
{"confidence":1.0,"evidence_crop_refs":["crop_000001"],"normalized_text":"subto","raw_text":"SUBTO","source_frame_ids":["frame_000016","frame_000023"],"text_observation_id":"text_obs_000001","text_region_id":"text_region_000001"}
```

Exported record:

```json
{"confidence":1.0,"evidence_crop_refs":["crop_000001"],"normalized_text":"subto","raw_text":"SUBTO","source_frame_ids":["frame_000016","frame_000023"],"text_observation_id":"text_obs_000001","text_region_id":"text_region_000001","svp_export":{"evidence_crop_refs":[{"file":"layers/text/evidence_crops/crop_000001.jpg","found":true,"id":"crop_000001"}],"source_frame_ids":[{"found":true,"frame_index":15,"id":"frame_000016","pts_us":0},{"found":true,"frame_index":22,"id":"frame_000023","pts_us":7235991}]}}
```

The `svp_export` object's own keys are sorted.

## 6. Binary block streams

Depth (`spatial/depth.blocks.svpdz`), mask (`spatial/masks.blocks.svpmz`), and
embedding (`embeddings/embeddings.blocks.svpez`) payloads are SVPB block streams
(SVP RC2 Section 14.5): 160-byte little-endian headers followed by
Zstandard-compressed payloads. Each stream is exported as two files.

### 6.1 Representation choice

The export writes each block's **decompressed payload bytes, unchanged**,
concatenated in stream order into `<entry>.decoded.bin`, and describes every
block in `<entry>.blocks.jsonl`.

Why this form:

1. It is lossless and is not a reinterpretation. Decompressing is the only
   step; the values are the package's values in the package's own dtype.
2. SVPB declares little-endian payloads (`endian = 0x01`; other values are
   refused), so for numeric dtypes the decoded bytes already are plain
   little-endian arrays that any language can map directly, for example
   `numpy.fromfile(path, dtype="<u2", offset=decoded_offset, count=n)` or
   `numpy.frombuffer(..., dtype="<f4")`.
3. It avoids converting numbers to JSON text, which would multiply size (a 640x360
   depth map is 460,800 bytes decoded; as JSON numbers it would be several
   times larger) and would invite float formatting differences.
4. It keeps one file per stream instead of one file per block, so thousands of
   embedding vectors do not become thousands of files.

Every block is verified before it is written: header magic, version, size,
compression, endianness, reserved bytes, header BLAKE3, payload BLAKE3, strict
block type rules, and exact Zstandard decompression to the declared
`uncompressed_size`. These are the checks in `svp::blocks::parse_block_stream`;
decompression uses `svp::blocks::decompress_block_payload`, which bounds each
block at `svp::blocks::kMaxBlockPayloadBytes`. A stream that fails any check is
refused with `invalid_block_stream`.

### 6.2 Block table record

One line per block, in stream order:

```json
{"block_length":386815,"block_offset":0,"block_ordinal":0,"block_type":1,"block_type_name":"depth","compressed_size":386655,"compression":1,"decoded_file":"layers/spatial/depth.blocks.svpdz.decoded.bin","decoded_length":460800,"decoded_offset":0,"dtype":2,"dtype_name":"uint16","end_us":62022805,"endian":1,"extent_0":640,"extent_1":360,"extent_2":1,"flags":0,"frame_count":1,"header_blake3":"<64 hex>","header_size":160,"payload_blake3":"<64 hex>","start_frame":0,"start_us":62022804,"uncompressed_size":460800,"version":1}
```

| Field | Meaning |
| --- | --- |
| `block_ordinal` | Zero-based position of the block in the stream. |
| `block_offset`, `block_length` | Byte range of the block (header plus compressed payload) in the package stream, as index records reference it. |
| `version`, `header_size`, `block_type`, `compression`, `endian`, `flags`, `uncompressed_size`, `compressed_size`, `extent_0`..`extent_2`, `dtype`, `start_frame`, `frame_count`, `start_us`, `end_us` | Header fields as declared, as integers. `start_frame` is `18446744073709551615` (`UINT64_MAX`) for non-frame blocks and `start_us`/`end_us` are `-1` when not time-bound, exactly as SVPB declares. |
| `payload_blake3`, `header_blake3` | Header hashes, 64 lowercase hex characters. |
| `block_type_name` | `depth`, `mask`, or `embedding`. |
| `dtype_name` | `uint8`, `uint16`, `float16`, `float32`, `bitpacked_lsb_first`, or `svp-rle-v1`. |
| `decoded_file`, `decoded_offset`, `decoded_length` | Where this block's decompressed payload sits in `<entry>.decoded.bin`. `decoded_length` equals `uncompressed_size`. |

### 6.3 Reading decoded payloads

| Layer | dtype | Decoded layout |
| --- | --- | --- |
| Depth | `uint16` | `extent_0` (width) x `extent_1` (height) x `frame_count` little-endian `uint16`, row-major from the top-left. `0` is the farthest and `65535` the nearest valid relative depth in that frame (RC2 Section 14.4). |
| Embedding | `float32` | `extent_0` vectors of `extent_1` little-endian IEEE-754 `float32` values, vector after vector. |
| Mask | `svp-rle-v1` | Unsigned LEB128 run lengths, starting with a background run, alternating background and foreground, row-major from the top-left; runs total `extent_0 * extent_1` per plane (RC2 Section 14.3). |
| Mask | `bitpacked_lsb_first` | One bit per pixel, row-major, least significant bit first, `ceil(extent_0 * extent_1 * extent_2 / 8)` bytes. |

The export does not expand masks to full rasters: RLE is the package's declared
mask representation, it is lossless, and expanding it would multiply size by
the raster area for every mask.

The index records that point at blocks (`spatial/depth.index.jsonl`,
`spatial/masks.index.jsonl`, `embeddings/embeddings.index.jsonl`) are exported
as JSONL layers with a `svp_export.block_file` resolution (Section 5.2), so a
reader goes from an index record straight to its decoded bytes. The export does
not interpret `vector_index` or any other index field; those stay as declared.

## 7. What each common layer looks like

The table lists layers a typical SVPI from this builder holds, with the export a
consumer reads. Every layer is exported the same generic way; this is a guide,
not an allow-list. Unknown entries (for example `extensions/<namespace>/...`)
are exported by the same rules.

| Package entry | Exported as | Notes |
| --- | --- | --- |
| `manifest.json` | `layers/manifest.json` + `export.json` `manifest` | Format, versions, section states, timebase. |
| `media_binding.json` | `layers/media_binding.json` + `export.json` `media_binding` | SVPI only. Media identity, streams, `duration_us`. |
| `transcript/words.jsonl` | records | Word `text`, `start_us`, `end_us`, `speaker_id`, `confidence`, as declared. |
| `transcript/speakers.jsonl` | records | Speakers as declared. |
| `transcript/speaker_segments.jsonl` | records | `speaker_id`, `start_us`, `end_us`. |
| `transcript/transcript.json` | document | Transcript summary as declared. |
| `text/text_regions.jsonl` | records | Region boxes and `start_us` / `end_us`. |
| `text/text_observations.jsonl` | records + `svp_export` | `source_frame_ids` resolved to frame `pts_us` (the sample times the observation was seen at); `evidence_crop_refs` resolved to crop image files. |
| `text/evidence_crops.jsonl` | records + `svp_export` | `crop_file_path` resolved to `layers/text/evidence_crops/...`; `source_frame_id` resolved to its frame. |
| `text/evidence_crops/*.jpg` | files | Crop images, byte for byte. |
| `text/numeric_values.jsonl` | records | Numeric values as declared decimal strings. |
| `timeline/frames.jsonl` | records | Frame `id`, `frame_index`, `pts_us`. The lookup source for frame references. |
| `timeline/shots.jsonl`, `timeline/scenes.jsonl` | records (+ `svp_export` for shots) | |
| `provenance/processors.jsonl` | records | Processor provenance as declared, including any sampling schedule a processor declares (for example an OCR detector's `temporal_sampling` with `sampled_timestamps_us`, `max_sample_gap_us`, and coverage notes). The export copies it; it does not summarize or check it. |
| `provenance/interlace_events.jsonl`, `provenance/validation.json` | records / document | As declared. `provenance/validation.json` is the report stored by the producer, not the export's own validation run (that is in `export.json` `validation`). |
| `spatial/depth.blocks.svpdz` | block table + decoded payloads | Section 6. |
| `embeddings/embeddings.blocks.svpez` | block table + decoded payloads | Section 6. |
| `index/index.sqlite` | file | Copied byte for byte. It is an acceleration structure; the JSON, JSONL, and block layers stay authoritative. |
| `media/original/*` (SVP only) | file | The packaged source media, byte for byte. |

## 8. Determinism and safety

Determinism: the same package exported by the same build always produces
byte-identical files. Entries are processed in sorted entry-name order; JSON
written by the export uses sorted keys; nothing written depends on time, paths,
locale, thread count, or the host.

Safety:

1. **Entry names.** Every entry name must be a relative `/`-separated path with
   no empty, `.` or `..` segment, no backslash, and no control character, and
   every segment of every output path (including the `.blocks.jsonl` and
   `.decoded.bin` suffixes) must fit the platform's `NAME_MAX`. The validators
   already reject traversal names (`ERR_CORE_PATH_TRAVERSAL`); the export checks
   again (`entry_path_policy.cpp`) and refuses `unsafe_entry_name`.
2. **Collisions.** Two outputs that would land on the same file on a
   case-insensitive file system, or a file that would need to be a directory
   for another output, are refused with `output_path_collision`. Files are
   created with exclusive-create and no-follow flags inside a fresh staging
   directory, so nothing can be written through a link or over another file.
3. **Bounded reading.** Entries are streamed in fixed-size chunks and never
   decompressed past their declared size. An entry that yields more or fewer
   bytes than its declared size is refused (`entry_size_mismatch`). One JSONL
   record or one JSON document larger than `kMaxRecordBytes`
   (`jsonl_line_reader.hpp`, 64 MiB) is refused (`record_too_large`); that is
   the most the export holds in memory at once. Block payloads are bounded by
   `svp::blocks::kMaxBlockPayloadBytes`. Before writing, the export compares the
   package's declared uncompressed size with the free space of the destination
   file system and refuses `insufficient_space` rather than filling the disk.
4. **Unchanged input.** The input's file identity (device, inode, size,
   modification and change times) is recorded before validation and checked
   again before the export is published; a change is refused with
   `package_changed`.

## 9. Command result and exit codes

stdout always carries one JSON object (see
`spec/schemas/package-export-result.schema.json`).

Success:

```json
{"exit_code": 0, "export_format_version": 1, "file_count": 1970, "layer_count": 52, "out": "<--out as given>", "package_kind": "svpi", "status": "exported"}
```

Failure:

```json
{"error": {"code": "package_invalid", "details": {"validation": {"...": "full validation report"}}, "message": "..."}, "exit_code": 3, "status": "error"}
```

| Exit code | `error.code` values | Meaning |
| ---: | --- | --- |
| 0 | - | Export written. |
| 1 | `internal_error`, `validation_unavailable` | Unexpected failure, or the validator's registries and schemas could not be loaded. |
| 2 | `input_missing`, `input_not_regular_file`, `input_unrecognized` | The input is not a readable SVP, SVPI, or ISO BMFF file. |
| 3 | `package_invalid` | Validation failed. `details.validation` is the full validation report, as `svp-validator validate --json` prints it. |
| 4 | `unsupported_version` | A declared version is outside what this build supports. `details` names the `field`, the `declared` value, and the `supported` values. |
| 5 | `output_exists`, `output_not_replaceable`, `insufficient_space`, `output_write_failed` | `--out` cannot be used or written. |
| 6 | `unsafe_entry_name`, `output_path_collision`, `entry_unreadable`, `entry_size_mismatch`, `malformed_record`, `malformed_document`, `record_too_large`, `invalid_block_stream`, `reserved_member_present`, `package_changed` | The package passed validation but cannot be exported faithfully. `details` names the `entry` (and `line` for records). |

Command-line usage errors (for example a missing `--out`) are reported by the
argument parser on stderr with its own non-zero exit code before any export
logic runs.

For every non-zero exit, nothing is written to `--out`.

## 10. Versioning

`export_format_version` is `1`. A change that removes or renames a field,
changes a field's meaning, or changes a file layout increments it. Adding a
field, a reference rule, or a layer kind does not; readers must ignore what
they do not know.
