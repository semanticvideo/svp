# SVPI v0.1 Draft Specification

Status: Draft for maintainer review  
Format name: Semantic Video Package Interlace  
Short name: SVPI  
Recommended extension: `.svpi`  
Recommended container: ZIP64 package  
Recommended MIME type: `application/vnd.svp.interlace+zip`  
Target compatibility: SVP v1.0 RC2 observation model  

This document is a first-pass technical specification for SVPI v0.1. It is not a final standard. It defines a practical sidecar format that can be implemented, validated, criticized, and refined inside the SVP codebase.

SVPI stands for Semantic Video Package Interlace. An SVPI file stores the SVP-compatible package core for a source video while leaving the source media untouched. It interlaces semantic records, provenance, indexes, validation metadata, and media binding with a specific media identity rather than embedding the media bytes.

```text
SVP  = packaged media + semantic observations
SVPI = media-bound semantic observations without packaged primary media
```

SVPI does not replace SVP. SVP remains the self-contained package format for interchange, validation, querying, and traversal. SVPI adds a sidecar workflow for existing media libraries, editing workflows, camera output folders, capture hardware, and tools that should preserve the original media file exactly as it exists.

## 1. Design Summary

SVPI v0.1 is a ZIP64 sidecar package with the SVP core layout, minus `media/original/` primary media bytes. It contains a required `manifest.json`, a required `media_binding.json`, required provenance, required package/index spine files, and required local search index files under `index/`. The binding file proves which source media the sidecar belongs to using media identity fields such as content hash, file size, duration, stream metadata, and stream fingerprints.

The central rule is:

```text
Path = hint.
Media identity = truth.
```

A valid SVPI may be inspectable and searchable without the source media present. In that state it is structurally valid but unbound. To recombine a source video and an SVPI into a valid `.svp`, the source video MUST verify against the SVPI media binding. A packager MUST NOT silently combine an SVPI with unrelated media.

The recommended v0.1 workflow set is:

```text
Video -> Builder -> SVP
Video -> Builder -> SVPI
Video + SVPI -> Builder/Packager -> SVP
SVP -> Extractor -> Video + SVPI
Capture Device -> Video + SVPI    reserved for future camera-native use
```

SVPI reuses SVP observation concepts wherever possible, including transcript, timeline, entities, spatial data, visible text, numeric values, colors, relationships, embeddings, indexes, provenance, and validation output. The difference is not semantic meaning. The difference is storage, transport, and media binding. The minimum valid SVPI is therefore not a loose metadata bookmark; it is the valid SVP core minus embedded primary media, plus binding proof to that external media.

## 2. Terminology

The key words `MUST`, `MUST NOT`, `REQUIRED`, `SHOULD`, `SHOULD NOT`, `MAY`, and `OPTIONAL` are to be interpreted as normative requirement levels in this draft.

`SVP` means Semantic Video Package, the self-contained package format that embeds primary media and semantic observations.

`SVP core` means the non-primary-media package spine needed for SVP-compatible validation, search, provenance, traversal, and semantic records.

`SVPI` means Semantic Video Package Interlace, the media-bound SVP core sidecar format defined by this draft.

`source media` means the primary video or media file that an SVPI describes.

`primary media bytes` means the full bytes of the source media file. An SVPI v0.1 MUST NOT contain these as a replacement for the source media.

`media binding` means the set of required and optional identity fields used to verify that a candidate media file is the source media for a given SVPI.

`location hint` means a filename, relative path, absolute path, volume name, catalog key, or similar convenience value that may help software find the source media. A location hint is never proof of identity.

`bound` means a candidate media file has been supplied and verified against the SVPI media binding.

`unbound` means the SVPI is structurally valid, but no candidate source media has been supplied or verified.

`binding mismatch` means candidate media was supplied but failed media identity verification.

`recombination` means creating a full `.svp` package from source media plus `.svpi`.

`extraction` means creating source media plus `.svpi` from an existing `.svp` package.

`observation` means an SVP-compatible record about what is present, measured, detected, or derived from the source media over time.

`label` means an interpretation or classification. Labels are not the core observation model.

`evidence artifact` means a compact derived artifact, such as an OCR crop, preview thumbnail, waveform summary, or diagnostic sample. Evidence artifacts MAY be stored in SVPI, but they do not replace source media.

`authority` means the origin class for an observation, such as capture-time device observation, builder-derived observation, package-extracted observation, user-imported observation, or provider extension observation.

## 3. Goals

SVPI v0.1 has the following goals.

1. Preserve semantic observations alongside existing media files without modifying those media files.
2. Bind sidecar observations to a specific source media identity, not to an absolute path.
3. Reuse the SVP core package spine, observation model, and record formats wherever practical.
4. Enable reversible workflows between source media plus SVPI and full SVP packages.
5. Support local-first validation, inspection, recombination, and extraction.
6. Provide a required local search database in every conforming sidecar.
7. Preserve provenance for every observation and every transformation event.
8. Leave future extension points for camera-native capture and provider-specific observations.
9. Avoid conflating semantic sidecars with authenticity signatures.
10. Avoid requiring a cloud service, catalog service, or library index for the core format.

## 4. Non-Goals For v0.1

SVPI v0.1 does not define a cloud service, synchronization protocol, media editing format, project-file format, or content management system.

SVPI v0.1 does not require camera-native hardware capture. It reserves the concept for future versions.

SVPI v0.1 does not replace `.svp` packages.

SVPI v0.1 does not define a full authenticity or signature scheme. It may carry fields that are useful to later authenticity systems, but `.svpi` is not the same as `.svpsig`.

SVPI v0.1 does not make path hints authoritative.

SVPI v0.1 does not allow recombination with unrelated media.

SVPI v0.1 does not redefine SVP observations as labels.

SVPI v0.1 does not require a global library catalog.

SVPI v0.1 does not require the source media to be writable or modified.

## 5. Relationship To SVP

SVP remains the primary packaged representation. A `.svp` package is self-contained and includes the primary media bytes under the package's media section. A `.svpi` sidecar is not self-contained because it omits those primary media bytes.

A conforming SVPI SHOULD reuse SVP section names, record formats, IDs, timestamp semantics, provenance references, block streams, index concepts, traversal concepts, and validation concepts unless this specification explicitly defines a sidecar-specific difference.

The minimum conceptual difference is:

```text
SVP:
  contains manifest
  contains primary media
  contains SVP core records
  contains provenance
  contains indexes
  contains validation/traversal/search spine
  can be opened alone

SVPI:
  contains manifest
  contains media binding
  omits primary media
  contains SVP core records
  contains provenance
  contains indexes
  contains validation/traversal/search spine
  requires matching source media for recombination
```

The shorthand rule is:

```text
minimum valid SVPI = valid SVP core - embedded primary media + media binding
```

A valid SVPI may have no rich observations yet only when the SVP-compatible core records honestly represent the missing observations through section state, absence, blocker, or not-generated records. Such a sidecar is still useful as a binding artifact, extraction target, searchable catalog entry, or future enrichment target, but it cannot answer semantic queries beyond its stored core records.

## 6. Relationship To `.svpsig`

`.svpi` and `.svpsig` are different artifact types.

```text
.svpi   semantic observation sidecar bound to source media
.svpsig detached authenticity or signature sidecar for package bytes or a canonical signature payload
```

An SVPI MAY participate in future authenticity workflows, such as capture-device attestation, signed media binding, C2PA-compatible references, or package signatures. Those features are not required in v0.1.

A validator MUST NOT treat an `.svpi` as an authenticity signature. A validator MUST NOT treat an `.svpsig` as semantic observations.

## 7. Container Format

### 7.1 Recommendation

SVPI v0.1 SHOULD use a ZIP64 package container with `.svpi` file extension.

This draft recommends ZIP64 for v0.1 because it aligns with SVP package mechanics, supports JSON and JSONL records, supports binary block streams, supports optional evidence artifacts, permits single-file sidecar transport, and makes recombination into `.svp` straightforward.

### 7.2 Required ZIP Properties

An SVPI ZIP package MUST contain a root entry named `mimetype`.

The `mimetype` entry MUST contain exactly this UTF-8 string with no trailing newline:

```text
application/vnd.svp.interlace+zip
```

The `mimetype` entry SHOULD be the first ZIP entry and SHOULD be stored without compression. A v0.1 validator MAY warn when this is not true. A stricter future profile MAY require it.

All ZIP entry names MUST use forward slash separators. Entry names MUST NOT be absolute paths. Entry names MUST NOT contain `..` path traversal segments. Duplicate entry names MUST be invalid.

A reader MUST treat the ZIP container as untrusted input. It MUST protect against path traversal, decompression bombs, over-large entries, and malformed central directory data.

### 7.3 Alternative Containers Considered

Directory or bundle representation is easy for development and diffing, but less portable as a sidecar artifact.

Single JSON or JSONL is simple, but it does not fit large embeddings, masks, depth blocks, indexes, evidence crops, or future provider extensions.

SQLite-centric sidecar storage is efficient for queries, but it makes archival inspection harder and diverges from SVP's package model.

External block files can scale, but they create a multi-file sidecar family and complicate movement through existing media workflows.

Therefore, ZIP64 is the recommended v0.1 direction. A development tool MAY expose an unpacked directory representation, but the normative transport artifact for v0.1 is `.svpi`.

## 8. Package Layout

### 8.1 Recommended Root Layout

A typical SVPI v0.1 package SHOULD use this layout:

```text
mimetype
manifest.json
media_binding.json
media_binding/
  chunks_blake3.jsonl
transcript/
  words.jsonl
  speakers.jsonl
  speaker_segments.jsonl
timeline/
  frames.jsonl
  shots.jsonl
  scenes.jsonl
entities/
  entities.jsonl
  entity_tracks.jsonl
spatial/
  regions.jsonl
  masks.index.jsonl
  masks.blocks.svpmz
  depth.index.jsonl
  depth.blocks.svpdz
text/
  text_regions.jsonl
  text_observations.jsonl
  numeric_values.jsonl
  text_absence.json
  evidence_crops/
colors/
  color_observations.jsonl
  color_summary.json
  color_absence.json
relationships/
  relationships.jsonl
  relationship_provenance.json
embeddings/
  embeddings.index.jsonl
  embeddings.blocks.svpez
index/
  index.sqlite
  index_manifest.json
provenance/
  processors.jsonl
  model_hashes.jsonl
  interlace_events.jsonl
  validation.json
extensions/
  <namespace>/
```

Not every section must be present in every SVPI. Section presence is declared in `manifest.json`.

### 8.2 Required Entries

A structurally valid SVPI v0.1 package MUST contain:

```text
mimetype
manifest.json
media_binding.json
provenance/processors.jsonl
provenance/interlace_events.jsonl
index/index.sqlite
index/index_manifest.json
```

These entries are the minimum SVPI package spine. A conforming profile MAY also require additional SVP core files for the target compatibility profile, including timeline, relationship, validation, absence, blocker, traversal, or section-summary records.

`provenance/processors.jsonl` MAY be empty only for a core-only file produced by a tool that records the producing tool in `manifest.json` and honestly declares all semantic sections as not generated, absent, or blocked. Once observations are present, the processor records MUST identify the processors that created them.

`provenance/interlace_events.jsonl` MUST include at least one event describing the creation, extraction, or import of the SVPI artifact.

### 8.3 Forbidden Primary Media

An SVPI v0.1 package MUST NOT contain the full source media as replacement primary media.

The following path is forbidden for primary media bytes in SVPI v0.1:

```text
media/original/
```

A validator MUST fail an SVPI if it appears to contain a complete primary media file under `media/original/`.

### 8.4 Derived Media And Evidence Artifacts

An SVPI MAY contain small derived evidence artifacts, including OCR crops, diagnostic thumbnails, waveform summaries, low-resolution previews, or compact sampling records. These artifacts MUST be represented as derived evidence, not as source media.

Recommended locations are:

```text
text/evidence_crops/
evidence/
media/derived/
```

Evidence artifacts MUST include provenance and references back to source media time ranges or regions. Evidence artifacts MUST NOT be accepted as proof that the sidecar has the primary media bytes.

### 8.5 Index Files

`index/index.sqlite` and `index/index_manifest.json` are REQUIRED in every conforming SVPI v0.1.

`index/index_manifest.json` MUST describe the logical row set, schema version, source record references, and media binding expected by the index.

A conforming SVPI MUST be searchable without first being recombined into an `.svp` package. A minimal SVPI with no rich semantic observations may contain an index with zero observation rows, but the database and manifest MUST still exist and validate.

A recombined `.svp` package MUST satisfy the target SVP package's index requirements. The packager MAY copy a compatible SVPI index after validation, or it MAY rebuild the index from observation records.

A sidecar without `index/index.sqlite` is not a conforming SVPI v0.1 artifact. Tools MAY create such artifacts only as explicitly marked temporary, intermediate, repair, or diagnostic outputs.

The index is an acceleration structure, not the authoritative semantic source. The JSON, JSONL, and block records remain authoritative.

## 9. Manifest

SVPI v0.1 uses `manifest.json`, not `svpi_manifest.json`.

This aligns SVPI with SVP and keeps tooling paths simple. The manifest distinguishes the format through a required `format` field.

This decision is approved for v0.1. A conforming SVPI writer MUST use `manifest.json` with `format: "svpi"` rather than introducing `svpi_manifest.json`.

### 9.1 Required Manifest Fields

`manifest.json` MUST be a UTF-8 JSON object.

It MUST contain:

```text
schema
format
svpi_version
interlace_id
created_utc
media_binding_ref
primary_media_binding_id
svp_compatibility
source_timeline
sections
provenance
```

`format` MUST equal `svpi`.

`svpi_version` MUST equal `0.1` for this draft.

`media_binding_ref` MUST point to `media_binding.json` unless a future version defines a different binding location.

`primary_media_binding_id` MUST reference a binding inside `media_binding.json`.

`sections` MUST declare each included observation section and SHOULD declare known sections that were intentionally not generated.

### 9.2 Section States

The following section states are defined for v0.1:

`present`: Records for this section are present.

`declared_absent`: The section contains an absence record explaining why no records exist.

`not_generated`: No attempt was made to generate this section.

`blocked`: Generation was attempted but could not complete because of a blocker.

`unsupported`: The producing tool does not support this section.

`omitted`: The section was intentionally omitted from this sidecar.

For text and color observations, if a tool attempted the stage and produced no records, the tool SHOULD write the same absence records used by SVP RC2, such as `text/text_absence.json` and `colors/color_absence.json`.

A recombination tool targeting SVP v1.0 RC2 MUST ensure that the resulting `.svp` satisfies RC2 package requirements. If required text or color records are missing, the recombination tool MUST either generate valid records, generate valid absence records, or fail recombination.

### 9.3 Manifest Example

The following JSON is illustrative. Hash values are examples, not real media hashes.

```json
{
  "schema": "svpi.manifest.v0.1",
  "format": "svpi",
  "svpi_version": "0.1",
  "interlace_id": "svpi_01jexample8zw7h3k9t2m4p6",
  "created_utc": "2026-06-29T18:42:00Z",
  "created_by": {
    "tool_name": "svp-builder",
    "tool_version": "1.0.0-rc2-dev",
    "implementation": "cpp"
  },
  "media_binding_ref": "media_binding.json",
  "primary_media_binding_id": "mb_primary_000001",
  "svp_compatibility": {
    "target_profile": "svp.v1.0-rc2",
    "observation_model": "svp.observations.rc2",
    "requires_recombination_index": true
  },
  "source_timeline": {
    "timebase": "microseconds",
    "zero_point": "source_media_start",
    "duration_us": 123456789,
    "timestamp_semantics": "svp_source_media_time_us"
  },
  "canonical_analysis": {
    "raster_width": 1920,
    "raster_height": 1080,
    "sample_rate_fps": "30000/1001"
  },
  "sections": {
    "transcript": {
      "state": "present",
      "path": "transcript/"
    },
    "timeline": {
      "state": "present",
      "path": "timeline/"
    },
    "entities": {
      "state": "not_generated",
      "path": "entities/"
    },
    "spatial": {
      "state": "not_generated",
      "path": "spatial/"
    },
    "text": {
      "state": "present",
      "path": "text/"
    },
    "colors": {
      "state": "present",
      "path": "colors/"
    },
    "relationships": {
      "state": "not_generated",
      "path": "relationships/"
    },
    "embeddings": {
      "state": "not_generated",
      "path": "embeddings/"
    },
    "index": {
      "state": "present",
      "path": "index/",
      "sqlite_ref": "index/index.sqlite",
      "manifest_ref": "index/index_manifest.json"
    }
  },
  "provenance": {
    "processors_ref": "provenance/processors.jsonl",
    "model_hashes_ref": "provenance/model_hashes.jsonl",
    "events_ref": "provenance/interlace_events.jsonl",
    "validation_ref": "provenance/validation.json"
  },
  "extensions": []
}
```

## 10. Media Binding

### 10.1 Binding Principle

An SVPI is bound to media identity, not to path.

A validator MUST NOT consider an SVPI bound merely because the source media was found at an absolute path stored in the sidecar.

A validator MUST NOT treat an absolute path mismatch as a binding failure by itself.

A recombination tool MUST verify source media identity before producing a valid `.svp`.

### 10.2 Required Media Binding Fields

`media_binding.json` MUST be a UTF-8 JSON object.

It MUST contain:

```text
schema
primary_binding_id
bindings
```

For v0.1, `bindings` MUST contain exactly one binding whose `media_role` is `primary_source`. Future versions MAY support multiple primary or synchronized media inputs.

Each binding MUST contain:

```text
binding_id
media_role
media_id
duration_us
size_bytes
container_format
streams
binding_contract
verification_state
identity
location_hints
```

`binding_contract` MUST equal `svpi.media_identity.v0.1` for this draft.

`identity` MUST contain the media identity fields defined by the binding contract. For v0.1, that includes deterministic fast-match identity fields and a full-file hash field whose state is explicitly recorded.

### 10.3 Single Media Binding Contract

SVPI v0.1 has one media binding contract:

```text
svpi.media_identity.v0.1
```

Tools MUST NOT expose multiple user-facing binding levels such as weak, medium, strong, fast, or trusted. A user should not need to know which internal checks are available after creating an SVPI. A conforming SVPI has one media identity record and one verification state.

The contract records enough information for tools to verify that a candidate file is the source media without relying on filename, path, folder placement, or extension.

The v0.1 media identity contract contains:

1. File size.
2. Duration.
3. Container format.
4. Stream metadata.
5. Deterministic chunk proof for scalable matching.
6. Full-file BLAKE3 hash state.
7. Optional stream fingerprints when available.

The full-file BLAKE3 field MUST use one of these states:

`present`: A full-file BLAKE3 hash was computed and recorded.

`pending`: The SVPI was created before full-file hashing completed. The sidecar is structurally valid, but tools may need to finish hashing before final recombination.

`unavailable`: Full-file hashing could not be completed. The reason MUST be recorded.

`verification_state` describes the result of checking a candidate media file against the single binding contract. The allowed values are:

`verified`: The candidate media satisfies the binding contract.

`pending`: Verification has not completed, usually because full-file hashing is still pending.

`mismatch`: The candidate media does not satisfy the binding contract.

`unavailable`: The candidate media is unavailable or cannot be read.

If full-file BLAKE3 is `present`, it MUST match during verification. If it mismatches, verification MUST report `mismatch` even when filename, metadata, or chunk proof appear to match.

If full-file BLAKE3 is `pending`, a recombination tool SHOULD compute it before producing a final SVP. If it cannot compute the full hash, it MUST either leave verification `pending`, fail recombination, or produce an explicitly diagnostic output according to tool policy. It MUST NOT silently downgrade to filename or path matching.

Metadata-only matching is never sufficient for recombination.

### 10.4 Recommended Identity Fields

The required media identity fields for v0.1 are:

1. Size in bytes.
2. Duration in microseconds.
3. Container format and codec metadata.
4. Deterministic chunk hash manifest for scalable matching.
5. Full BLAKE3 content hash state.
6. Stream fingerprints for video, audio, subtitle, and timecode streams when available.

Hash algorithms SHOULD be named explicitly. BLAKE3 is preferred for SVP-family tooling. SHA-256 MAY be included as a secondary compatibility hash.

### 10.5 Location Hints

Location hints MAY include original filename, relative path, original absolute path, volume hint, library ID, catalog key, or last seen timestamp.

Location hints are convenience data. They are not identity proof.

A validator SHOULD report location hints during inspection, but MUST clearly distinguish them from binding evidence.

### 10.6 Media Binding Example

```json
{
  "schema": "svpi.media_binding.v0.1",
  "primary_binding_id": "mb_primary_000001",
  "bindings": [
    {
      "binding_id": "mb_primary_000001",
      "media_role": "primary_source",
      "media_id": "media_01jexampleprimary000001",
      "binding_contract": "svpi.media_identity.v0.1",
      "verification_state": "verified",
      "duration_us": 123456789,
      "size_bytes": 987654321,
      "container_format": "mov",
      "identity": {
        "full_file_blake3": {
          "state": "present",
          "value": "blake3:b3_example_full_file_hash_not_real"
        },
        "alternate_hashes": [
          {
            "algorithm": "sha256",
            "value": "sha256_example_full_file_hash_not_real"
          }
        ],
        "chunk_hashes": {
          "algorithm": "blake3",
          "chunk_size_bytes": 16777216,
          "chunk_count": 59,
          "root": "b3_example_chunk_root_not_real",
          "hashes_ref": "media_binding/chunks_blake3.jsonl"
        }
      },
      "streams": [
        {
          "stream_id": "v:0",
          "stream_type": "video",
          "codec_name": "h264",
          "codec_profile": "high",
          "width": 1920,
          "height": 1080,
          "r_frame_rate": "30000/1001",
          "duration_us": 123456789,
          "fingerprints": [
            {
              "algorithm": "svp-stream-sample-blake3-v0.1",
              "value": "b3_example_video_stream_fingerprint_not_real"
            }
          ]
        },
        {
          "stream_id": "a:0",
          "stream_type": "audio",
          "codec_name": "aac",
          "sample_rate_hz": 48000,
          "channels": 2,
          "duration_us": 123440000,
          "fingerprints": [
            {
              "algorithm": "svp-stream-sample-blake3-v0.1",
              "value": "b3_example_audio_stream_fingerprint_not_real"
            }
          ]
        }
      ],
      "location_hints": {
        "original_filename": "video.mov",
        "relative_path": "./video.mov",
        "original_absolute_path": "/Volumes/DriveA/project/video.mov",
        "volume_hint": "DriveA",
        "last_seen_utc": "2026-06-29T18:41:58Z"
      }
    }
  ]
}
```

### 10.7 Chunk Hash Records

When `media_binding/chunks_blake3.jsonl` is present, each line SHOULD be a JSON object like this:

```json
{"chunk_index":0,"offset_bytes":0,"length_bytes":16777216,"algorithm":"blake3","value":"b3_example_chunk_000000_not_real"}
```

A validator supporting chunk verification MUST check chunk count, offsets, lengths, chunk hash algorithm, and root consistency.

## 11. Observation Model Reuse

SVPI records SHOULD use SVP record formats and directories wherever practical.

The following sections are inherited conceptually from SVP:

```text
transcript/
timeline/
entities/
spatial/
text/
colors/
relationships/
embeddings/
index/
provenance/
```

SVPI MUST NOT redefine observations as labels.

SVPI MAY include a `labels/` section only if labels are explicitly separated from core observations and declared as optional interpretation data.

### 11.1 IDs

When an SVPI is extracted from an SVP, observation IDs SHOULD be preserved exactly.

When an SVPI is generated directly from source media, IDs SHOULD be stable across repeated runs when inputs, processors, models, and configuration are equivalent.

ID generation MAY use deterministic prefixes, canonical time ranges, stream IDs, record type names, content hashes, or processor-specific stable keys. The exact algorithm may be defined by SVP core registries.

An observation ID MUST be unique within its declared record namespace.

Relationships MUST reference existing IDs or explicitly declare unresolved references.

### 11.2 Timestamps

SVPI observations MUST use the source media timeline unless a record explicitly declares a different time basis.

The default time unit is microseconds from source media start.

A record that refers to decoded frames, samples, shots, scenes, transcript words, regions, or embeddings SHOULD include enough timing information to map it back to the source media.

For variable frame rate media, timestamps SHOULD preserve decoded or container-derived media time rather than inventing constant frame numbers as the only time basis.

### 11.3 Media References

Observation records that refer to media SHOULD reference the `media_id` from `media_binding.json`.

An SVPI v0.1 with exactly one primary source MAY omit repeated media references in records when the containing section unambiguously applies to the primary binding, but explicit media references are preferred.

### 11.4 Relationships

Relationships in `relationships/relationships.jsonl` SHOULD preserve SVP relationship semantics.

A relationship record MUST NOT silently point to a missing object. It MUST either reference an existing record or declare that the relationship is unresolved, external, omitted, or invalid.

### 11.5 Equivalence

An extracted SVPI and a recombined SVP SHOULD preserve observation identity, timing, provenance, and relationships within the limits of container transformation.

Allowed differences MAY include package IDs, artifact creation timestamps, validation timestamps, recombination provenance events, index rebuild order, ZIP entry ordering, compression choices, and derived validation reports.

Semantic record content SHOULD remain equivalent.

## 12. Provenance

Every SVPI observation MUST have provenance.

Provenance may be direct, through a `provenance_id` on each record, or indirect, through section-level provenance declared in a section manifest. Direct record-level provenance is preferred for observations produced by multiple processors or models.

### 12.1 Required Provenance Files

SVPI v0.1 requires:

```text
provenance/processors.jsonl
provenance/interlace_events.jsonl
```

SVPI v0.1 recommends:

```text
provenance/model_hashes.jsonl
provenance/validation.json
```

### 12.2 Processor Records

A processor record SHOULD identify the tool, version, configuration hash, input references, output sections, and relevant model references.

### 12.3 Interlace Events

`provenance/interlace_events.jsonl` records transformations involving the sidecar artifact itself.

Defined event types are:

`svpi_created_from_media`: The SVPI was generated from source media by a builder.

`svpi_extracted_from_svp`: The SVPI was extracted from a full SVP package.

`svp_recombined_from_svpi`: A full SVP package was created from source media and SVPI.

`svpi_enriched`: Additional observations were added to an existing SVPI.

`svpi_rebound`: Media binding was recalculated or strengthened.

`svpi_validated`: A validation report was produced.

### 12.4 Provenance Authorities

Observation authority values for v0.1 are:

`capture_time`: Observation came directly from capture hardware, capture software, sensor data, or device metadata.

`builder_derived`: Observation was derived later by SVP tooling, models, decoders, OCR, color sampling, transcription, or other analysis.

`package_extracted`: Observation was copied out of an SVP package into an SVPI sidecar.

`user_imported`: Observation was supplied or edited by a user or user-controlled import process.

`provider_extension`: Observation came from a declared extension namespace.

Tools MUST preserve provenance when observations are extracted, enriched, recombined, merged, or transformed.

Tools MUST NOT silently discard lower-ranked or conflicting observations. If two observations conflict, both SHOULD be preserved with provenance unless a user or policy explicitly chooses otherwise.

Capture-time observations may be more authoritative than later estimates. For example, camera-reported focal length generally outranks inferred focal length. Device-reported orientation generally outranks post-derived orientation. This is an authority preference, not permission to erase derived records.

### 12.5 Provenance Event Example

```json
{
  "schema": "svpi.provenance_event.v0.1",
  "event_id": "event_000001",
  "event_type": "svpi_created_from_media",
  "event_utc": "2026-06-29T18:42:00Z",
  "authority": "builder_derived",
  "tool": {
    "name": "svp-builder",
    "version": "1.0.0-rc2-dev",
    "configuration_hash": "b3_example_config_hash_not_real"
  },
  "inputs": [
    {
      "kind": "source_media",
      "media_binding_id": "mb_primary_000001",
      "media_id": "media_01jexampleprimary000001"
    }
  ],
  "outputs": [
    {
      "kind": "svpi",
      "interlace_id": "svpi_01jexample8zw7h3k9t2m4p6"
    }
  ],
  "notes": "Generated transcript, OCR, numeric values, color observations, and timeline records."
}
```

### 12.6 Extracted From SVP Provenance

An SVPI extracted from an SVP MUST record:

```text
source_svp_package_id
source_svp_format_version
source_svp_hash if available
extraction_tool_name
extraction_tool_version
extraction_utc
extracted_media_binding_id
extracted_media_id
preserved_observation_count by section when practical
```

Extraction MUST preserve original observation provenance where available and add extraction provenance as an additional transformation event.

### 12.7 Recombined Into SVP Provenance

A recombined SVP MUST record:

```text
input_source_media_identity
input_svpi_interlace_id
input_svpi_hash if available
binding_verification_result
packager_tool_name
packager_tool_version
recombination_utc
target_svp_profile
```

The recombined package MUST NOT pretend that sidecar-derived records were newly observed from media unless they were actually regenerated.

## 13. Validation

SVPI validation is divided into modes. A tool may run one or more modes depending on whether source media is available.

### 13.1 Structure Validation

Structure validation answers:

```text
Is this a well-formed SVPI package?
```

Structure validation MUST check:

1. File extension or declared artifact type when applicable.
2. ZIP readability.
3. Required `mimetype` entry.
4. Required `manifest.json`.
5. Required `media_binding.json`.
6. Required provenance files.
7. Required `index/index.sqlite`.
8. Required `index/index_manifest.json`.
9. Valid JSON and JSONL syntax for required records.
10. No invalid paths, duplicate paths, or traversal paths.
11. No forbidden primary media under `media/original/`.
12. Manifest `format` equals `svpi`.
13. Manifest version support.
14. Media binding schema support.
15. Section declarations match present files.
16. Observation records conform to declared SVP-compatible schemas when validators for those sections are available.
17. Index manifest matches index file and declared media binding.
18. Unknown required extensions are reported as unsupported.

### 13.2 Binding Validation

Binding validation answers:

```text
Does this SVPI match the supplied source media?
```

Binding validation requires candidate source media. It MUST check the media identity fields required by the single declared binding contract.

Binding validation MUST use the single `svpi.media_identity.v0.1` binding contract. It MUST NOT validate by filename, path, folder adjacency, extension, or location hint.

It MUST check size, duration, container format, stream metadata, deterministic chunk proof, and full-file BLAKE3 state according to the binding contract.

If full-file BLAKE3 is `present`, it MUST match. If full-file BLAKE3 is `pending`, validation MUST report the binding as pending unless the validator computes and records the full hash. Metadata-only matching is not sufficient for recombination.

### 13.3 Recombination Validation

Recombination validation answers:

```text
Can this SVPI and this source media produce a valid SVP for a target profile?
```

Recombination validation MUST require successful binding validation unless the output is explicitly marked invalid or diagnostic-only.

It MUST also check that the sidecar has enough records, absence records, or generation permissions to satisfy the target SVP profile.

### 13.4 Equivalence Validation

Equivalence validation answers:

```text
Is an extracted and recombined package equivalent to the original within allowed transformation rules?
```

Equivalence validation MAY compare:

1. Media content hashes.
2. Manifest media identity.
3. Observation record sets.
4. Relationship references.
5. Provenance preservation.
6. Index logical row sets.
7. Validation summaries.

Allowed differences are described in section 11.5.

### 13.5 Validation States

The top-level validation states are:

`valid_bound`: Structure is valid and supplied media verified.

`valid_unbound`: Structure is valid, but no candidate media was supplied or verified.

`invalid_structure`: Structure validation failed.

`binding_mismatch`: Structure may be valid, but supplied media failed binding verification.

`unsupported_version`: The validator does not support the declared SVPI version or required schema version.

`unsupported_required_extension`: The SVPI declares a required extension unknown to the validator.

`not_recombinable`: The SVPI may be structurally valid, but cannot produce a valid target SVP with the supplied inputs.

### 13.6 Validation Report Example

```json
{
  "schema": "svpi.validation_report.v0.1",
  "validated_utc": "2026-06-29T19:05:00Z",
  "validator": {
    "name": "svp-validator",
    "version": "1.0.0-rc2-dev"
  },
  "overall_state": "valid_bound",
  "structure": {
    "state": "valid",
    "errors": [],
    "warnings": []
  },
  "index": {
    "state": "valid",
    "sqlite_ref": "index/index.sqlite",
    "manifest_ref": "index/index_manifest.json"
  },
  "binding": {
    "state": "verified_full_hash",
    "media_binding_id": "mb_primary_000001",
    "candidate_media_path": "./video.mov",
    "checks": [
      {
        "name": "size_bytes",
        "state": "pass"
      },
      {
        "name": "content_blake3",
        "state": "pass"
      },
      {
        "name": "duration_us",
        "state": "pass"
      }
    ]
  },
  "recombination": {
    "state": "recombinable",
    "target_profile": "svp.v1.0-rc2",
    "required_actions": []
  }
}
```

## 14. Recombination Rules

Recombination creates a full `.svp` package from a source media file and an `.svpi` sidecar.

```text
source video + video.svpi -> video.svp
```

### 14.1 Required Recombination Steps

A recombination tool MUST:

1. Open and structure-validate the SVPI.
2. Load `media_binding.json`.
3. Load the candidate source media from an explicit argument or from location hints.
4. Verify the candidate media against the media binding.
5. Fail if binding verification fails.
6. Create a target SVP package using the requested SVP profile.
7. Copy the verified source media into the SVP package's primary media location.
8. Convert or copy compatible observation sections.
9. Preserve observation IDs where possible.
10. Preserve provenance and add recombination provenance.
11. Validate or rebuild the index as required by the target SVP profile.
12. Run target SVP validation or record why validation could not complete.

### 14.2 Binding Failure

If media binding fails, the tool MUST NOT produce a normal valid SVP.

The tool MAY produce a diagnostic artifact only if it is explicitly marked invalid, unbound, or diagnostic-only. Such an artifact MUST NOT be indistinguishable from a valid package.

### 14.3 Missing Required Sections

If a target SVP profile requires a section not present in the SVPI, the recombination tool has three options:

1. Generate the missing section from source media and record provenance.
2. Generate a valid absence or blocker record when allowed by the target profile.
3. Fail recombination.

The tool MUST NOT silently create empty required sections that imply successful observation.

### 14.4 Index Handling

The SVPI MUST include `index/index.sqlite` and `index/index_manifest.json`.

If the SVPI includes a compatible index, the recombination tool MAY copy it into the SVP after verifying the index manifest and media binding reference.

If the index is incompatible, stale, or uses a schema unsupported by the target SVP profile, the recombination tool MUST rebuild the SVP index from authoritative observation records before producing the final package.

If the index is missing, the input is not a conforming SVPI v0.1 artifact. A recombination tool MAY offer an explicit repair or diagnostic mode that rebuilds the missing index, but it MUST NOT silently treat a missing index as normal.

### 14.5 Provenance Handling

The recombined SVP MUST include a provenance event identifying:

```text
source media identity
input SVPI identity
binding verification result
packager tool
packager version
recombination timestamp
target SVP profile
```

Observation records copied from SVPI MUST retain their original observation provenance.

## 15. Extraction Rules

Extraction creates source media plus an `.svpi` from a full `.svp` package.

```text
video.svp -> video.mov + video.svpi
```

### 15.1 Required Extraction Steps

An extraction tool MUST:

1. Open and validate or inspect the source SVP package.
2. Extract the primary source media bytes to an output location.
3. Compute media binding identity for the extracted media.
4. Create an SVPI package with `manifest.json`, `media_binding.json`, and required provenance files.
5. Copy compatible observation sections from SVP into SVPI.
6. Preserve observation IDs where possible.
7. Preserve original observation provenance.
8. Add extraction provenance identifying the source SVP package.
9. Write `index/index.sqlite` and `index/index_manifest.json` by copying a compatible index or rebuilding it.
10. Produce a validation report for the extracted SVPI when practical.

### 15.2 Extracted Media Binding

An extracted SVPI MUST bind to the extracted source media, not merely to the original media path once seen by the SVP builder.

If the source SVP contains a media content hash, the extracted media hash SHOULD match it. The extraction tool SHOULD record both the original package media identity and the newly computed extracted media identity.

### 15.3 Package Identity Preservation

Extraction MUST record the source SVP package identity when available.

At minimum, extraction provenance SHOULD include:

```text
source_svp_package_id
source_svp_format_version
source_svp_hash
source_svp_manifest_hash
extraction_tool_name
extraction_tool_version
extraction_utc
```

### 15.4 Index Extraction

An extractor MUST write `index/index.sqlite` and `index/index_manifest.json` into the SVPI.

The extractor MAY copy a compatible SVP index after adapting it to the SVPI context, or it MAY rebuild the SVPI index from authoritative records.

If the index is copied, the extractor MUST ensure that it does not contain package-internal media paths or package IDs that would make it invalid in the SVPI context. The copied index MUST have a compatible `index/index_manifest.json`.

If the extractor cannot produce a valid index, extraction MUST fail or produce an explicitly marked non-conforming diagnostic artifact.

## 16. CLI Workflow Shape

The following CLI commands are recommended but not final.

### 16.1 Existing SVP Build

```bash
svp build video.mov --out video.svp
```

Produces a complete packaged `.svp` from source media.

### 16.2 Create SVPI Sidecar

```bash
svp interlace create video.mov --out video.svpi
```

Produces a media-bound `.svpi` sidecar. The source video remains untouched.

Useful options may include:

```bash
svp interlace create video.mov --out video.svpi --profile svpi.v0.1
svp interlace create video.mov --out video.svpi --stages timeline,text,colors
svp interlace create video.mov --out video.svpi --binding full-hash
```

`svp interlace create` MUST produce `index/index.sqlite` and `index/index_manifest.json` for conforming SVPI v0.1 output. A no-index output mode, if implemented, MUST be named and marked as temporary, repair, or diagnostic output rather than normal SVPI creation.

### 16.3 Validate SVPI Structure Only

```bash
svp interlace validate video.svpi
```

Validates the sidecar structure. If source media is not supplied, a structurally valid sidecar reports `valid_unbound`.

### 16.4 Validate SVPI Binding

```bash
svp interlace validate video.svpi --media video.mov
```

Validates structure and verifies media binding. A successful result reports `valid_bound`.

### 16.5 Inspect SVPI

```bash
svp interlace inspect video.svpi
```

Prints sidecar summary, media binding hints, section counts, validation status, and recombination readiness.

### 16.6 Recombine To SVP

```bash
svp interlace package video.mov video.svpi --out video.svp
```

Verifies media binding and creates a full `.svp` package.

Alternative command spelling for consideration:

```bash
svp interlace recombine video.mov video.svpi --out video.svp
```

`recombine` is semantically clearer. `package` aligns with the output artifact. The first implementation may support one and alias the other.

### 16.7 Extract SVPI From SVP

```bash
svp interlace extract video.svp --out-dir ./out
```

Produces source media and an `.svpi` sidecar. The extracted sidecar is bound to the extracted media.

## 17. Camera-Native SVPI Extension Point

Camera-native SVPI is future scope. It is not required in v0.1.

Future capture devices or capture applications may create:

```text
video.mov
video.svpi
```

at recording time.

Such a sidecar may contain direct capture-time observations, including:

```text
lens used
focal length
exposure duration
ISO
white balance
frame timing
orientation
device identity
depth streams
motion sensors
capture timestamps
calibration data
timecode
GPS when available and permitted
```

These observations are not necessarily AI-derived. They may be direct sensor or device records and can be more authoritative than later inference.

SVPI v0.1 reserves space for this through provenance authority, provider extensions, and extension namespace rules. It does not define a camera telemetry schema yet.

## 18. Provider Extensions

SVPI MAY contain provider-specific extensions under `extensions/`.

Recommended extension namespace forms are reverse DNS names or registered SVP extension names:

```text
extensions/com.apple.capture/
extensions/com.example.drone/
extensions/org.svp.experimental.depth/
```

Each extension SHOULD include:

```text
extensions/<namespace>/extension_manifest.json
```

### 18.1 Extension Manifest

An extension manifest SHOULD declare:

```text
namespace
schema_version
required
producer
records
core_mappings
provenance_refs
```

### 18.2 Extension Rules

Unknown optional extensions MUST NOT break core SVPI validation.

Unknown required extensions MUST produce `unsupported_required_extension` unless the validator can process them.

Extension records MUST carry provenance.

Extension records MUST NOT silently override core records.

If extension data is mapped into core SVP records, the mapped core records MUST carry provenance that points back to the extension source.

Extension names MUST NOT use reserved core root names.

### 18.3 Extension Manifest Example

```json
{
  "schema": "svpi.extension_manifest.v0.1",
  "namespace": "com.example.camera",
  "schema_version": "0.1",
  "required": false,
  "producer": {
    "name": "Example Camera App",
    "version": "0.9.0"
  },
  "records": [
    {
      "path": "extensions/com.example.camera/capture_metadata.jsonl",
      "record_type": "capture_metadata"
    }
  ],
  "core_mappings": [],
  "provenance_refs": [
    "provenance/interlace_events.jsonl"
  ]
}
```

## 19. Authenticity And Trust Considerations

SVPI v0.1 does not require signatures.

Future versions may support:

```text
capture-device attestation
signed media binding
C2PA-compatible references
signed extension manifests
signed package manifests
separate .svpsig files
```

A signed SVPI can still be semantically wrong. A signature can prove origin or integrity, not truth by itself.

A media binding hash can prove byte identity against a candidate file, but it does not prove authorship, date, or chain of custody unless paired with additional authenticity systems.

Validators SHOULD keep structure validity, binding validity, and authenticity status separate.

## 20. Library Search Considerations

SVPI is designed before library search, not as a library search system.

A future library index may scan mixed collections like:

```text
video_a.svp
video_b.mov + video_b.svpi
video_c.mp4 + video_c.svpi
```

and build a catalog across packages and sidecars.

That catalog is a software layer, not a core SVPI requirement.

SVPI should still expose enough stable identity to support such a catalog later:

```text
interlace_id
media_id
media content hash
source duration
stream fingerprints
section summaries
provenance summaries
optional original filename hints
```

## 21. Security And Privacy Considerations

SVPI files are untrusted inputs.

Readers MUST protect against malicious ZIP paths, duplicate entries, oversized decompression, malformed JSON, malformed JSONL, invalid SQLite files, and malicious extension payloads.

A tool MUST NOT execute code from an SVPI package.

A tool SHOULD treat `index/index.sqlite` as derived untrusted data. It SHOULD verify the index manifest before use and SHOULD prefer rebuilding when uncertain.

Media hashes can act as content fingerprints. They may reveal that a user possesses a known file. Tools MAY offer privacy modes that redact or weaken identity fields, but weakened identity fields reduce or remove recombination trust.

Location hints may expose user paths, drive names, project names, client names, or personal information. Tools SHOULD allow users to omit absolute paths from shared SVPI artifacts.

GPS, device identity, capture timestamps, and camera-native telemetry may be sensitive. Future camera-native profiles SHOULD define privacy controls.

## 22. Minimum Valid SVPI v0.1

The smallest structurally valid SVPI v0.1 is a searchable SVP core sidecar: the required SVP-compatible package spine minus embedded primary media, plus `media_binding.json`.

In shorthand:

```text
minimum valid SVPI = valid SVP core - media/original + media_binding.json
```

Minimum layout:

```text
mimetype
manifest.json
media_binding.json
provenance/processors.jsonl
provenance/interlace_events.jsonl
index/index.sqlite
index/index_manifest.json
```

The exact minimum file set MAY expand when the declared `svp_compatibility.target_profile` requires additional core records. For example, a profile may require timeline, relationship, traversal, validation, absence, blocker, or section-summary records even when no rich semantic observations were generated.

A minimum sidecar may have no successful transcript, OCR, color, entity, spatial, relationship, or embedding observations only if the corresponding SVP core records honestly declare the section state as not generated, absent, blocked, or unavailable according to the target compatibility profile. It MUST still include a valid index database and index manifest. The index may contain zero semantic observation rows, but it must be present so readers can rely on SVPI v0.1 as a searchable sidecar format.

Such a sidecar can be:

```text
structurally valid
valid_bound when matching media is supplied
not semantically rich
SVP-core-compatible except for embedded primary media
recombinable into an SVP when the external media verifies and the target SVP profile accepts its core/absence/blocker records
```

A core-only sidecar without an index is a temporary or diagnostic artifact, not a conforming SVPI v0.1 file.

## 23. Not In v0.1

The following are out of scope for v0.1:

1. Required camera-native hardware capture.
2. Multi-camera or multi-primary-media binding.
3. A complete capture telemetry schema.
4. A global extension registry.
5. A no-index conforming SVPI profile.
6. A required library or catalog index across multiple SVPI files.
7. Cloud sync, remote resolution, or account systems.
8. Mandatory signatures or attestation.
9. Media editing or project timeline semantics.
10. Automatic conflict resolution between observations.
11. Replacement of `.svp` packages.
12. Replacement of `.svpsig` authenticity sidecars.

## 24. Open Questions

### 24.1 Should SVPI use `manifest.json` or `svpi_manifest.json`?

Decision: use `manifest.json` with `format: "svpi"` for v0.1.

This keeps SVP-family tooling consistent and avoids a parallel manifest filename. The downside is that a user inspecting an unpacked sidecar cannot distinguish the format from the filename alone. The `format` field resolves this for tools.

### 24.2 Should SVPI be ZIP64 like SVP?

This draft recommends yes for v0.1. ZIP64 keeps the sidecar portable and aligned with SVP. The downside is that very large block streams can make sidecars heavy. A future profile could allow directory bundles or external block stores.

### 24.3 Should `index/index.sqlite` be required in SVPI v0.1?

This draft recommends yes. Search is a core reason SVPI exists, so every conforming v0.1 sidecar MUST include `index/index.sqlite` and `index/index_manifest.json`.

The index may be sparse for a minimal SVP-core sidecar, and tools may rebuild it when compatibility is uncertain. However, a missing index means the artifact is temporary, diagnostic, repair-mode, or non-conforming.

### 24.4 Should evidence crops be stored inside SVPI?

This draft allows them, especially OCR crops and QA thumbnails. They must remain bounded evidence artifacts and must not become replacement media.

### 24.5 Should depth, mask, and embedding block streams be allowed in SVPI?

This draft allows them if they use SVP-compatible block stream formats and carry provenance. They are observations or derived data, not primary media.

### 24.6 What is the minimum required SVPI for a sidecar generated from video?

This draft defines the minimum as SVP Core minus embedded primary media, plus media binding. At the package-spine level this includes at least `mimetype`, `manifest.json`, `media_binding.json`, required provenance files, `index/index.sqlite`, and `index/index_manifest.json`, plus any additional SVP core records required by the declared compatibility profile.

### 24.7 What validation state should represent a structurally valid SVPI whose media is missing?

This draft uses `valid_unbound`.

### 24.8 How strong must media binding be for normal local workflows?

This draft defines one media identity binding contract: `svpi.media_identity.v0.1`. The contract includes size, duration, container metadata, stream metadata, deterministic chunk proof for scalable matching, full-file BLAKE3 state, and optional stream fingerprints. Tools may verify progressively, but they must not expose multiple user-facing binding levels.

### 24.9 Should full media BLAKE3 be mandatory?

This draft requires the full-file BLAKE3 field to exist inside the single media identity contract, with state `present`, `pending`, or `unavailable`. Builders SHOULD compute it when practical and MAY complete it asynchronously for very large files. If present, it is authoritative and must match. Metadata-only binding is not recombinable.

### 24.10 How should extracted SVPI preserve original SVP package identity?

This draft requires extraction provenance to record source SVP package identity, source package hash when available, extraction tool, extraction timestamp, and preserved observation counts when practical.

### 24.11 How should camera-native fields be reserved without over-designing hardware capture now?

This draft reserves authority values, provider extension namespace rules, and future capture-time extension space. It does not define a camera telemetry schema in v0.1.

### 24.12 How should provider extensions declare whether they are optional or required?

This draft uses `extensions/<namespace>/extension_manifest.json` with a boolean `required` field. Unknown optional extensions do not break core validation. Unknown required extensions produce `unsupported_required_extension`.

### 24.13 Should validation output live under `validation/` or `provenance/validation.json`?

This draft recommends `provenance/validation.json` for alignment with current SVP package practice. A future version may introduce a dedicated `validation/` root if validation artifacts become multi-file.

### 24.14 Should SVPI support multiple source media files?

This draft reserves the structure by using a `bindings` array, but v0.1 requires exactly one `primary_source` binding. Multi-source editing timelines, multicam capture, and synchronized camera rigs are future scope.

## 25. Suggested Prototype Milestones

This section is non-normative. It exists to guide first implementation work.

Milestone 1: Add SVPI package probing and mimetype recognition.

Milestone 2: Add `manifest.json` and `media_binding.json` schemas.

Milestone 3: Add structure validation for required entries, paths, JSON, JSONL, required index files, and forbidden `media/original/`.

Milestone 4: Add minimal SVPI SQLite index and `index/index_manifest.json` writing, including a valid empty/sparse index for core-only sidecars.

Milestone 5: Add `svpi.media_identity.v0.1` binding creation and verification, including deterministic chunk proof and full-file BLAKE3 state.

Milestone 6: Add `svp interlace inspect` for manifest, binding, section states, provenance event summaries, and index readiness.

Milestone 7: Add `svp interlace create` for searchable SVP-core sidecars.

Milestone 8: Add text and color sidecar generation using existing SVP observation records.

Milestone 9: Add `svp interlace package` or `svp interlace recombine` to produce a valid SVP from source media plus SVPI.

Milestone 10: Add `svp interlace extract` to split an SVP into source media plus SVPI.

Milestone 11: Add equivalence checks for extract and recombine round trips.

## 26. Conformance Checklist

A conforming SVPI v0.1 writer SHOULD:

1. Write a ZIP64 `.svpi` file.
2. Write `mimetype` with `application/vnd.svp.interlace+zip`.
3. Write `manifest.json` with `format: "svpi"` and `svpi_version: "0.1"`.
4. Write `media_binding.json` with exactly one primary source binding.
5. Write the full `svpi.media_identity.v0.1` binding contract, including deterministic chunk proof and full-file BLAKE3 state.
6. Record size, duration, container format, and stream metadata.
7. Record location hints as hints only.
8. Write required provenance files.
9. Write the SVP core records required by the declared compatibility profile, except embedded primary media.
10. Preserve SVP observation formats.
11. Preserve provenance for every observation.
12. Avoid embedding primary media under `media/original/`.
13. Declare section states accurately.
14. Include absence or blocker records whenever required observations were attempted, unavailable, or intentionally not generated.
15. Write `index/index.sqlite` and `index/index_manifest.json` for every conforming SVPI.
16. Keep the index rebuildable from authoritative JSON, JSONL, and block records.
17. Add validation output when validation is run.

A conforming SVPI v0.1 reader SHOULD:

1. Validate structure before trusting records.
2. Treat all paths and SQL indexes as untrusted.
3. Distinguish structure validity from binding validity.
4. Never treat paths as media proof.
5. Verify media binding before recombination.
6. Preserve unknown optional extensions where possible.
7. Reject unknown required extensions.
8. Preserve observation IDs and provenance during extraction or recombination.
9. Require `index/index.sqlite` and `index/index_manifest.json` for conforming v0.1 structure.
10. Rebuild indexes when index compatibility is uncertain.
11. Keep authenticity status separate from structure and binding status.

## 27. One-Sentence Contract

An SVPI v0.1 file is a ZIP64 semantic sidecar that stores SVP-compatible observations and provenance for one specific source media identity, without containing the source media itself, and it can only be recombined into a valid SVP after that media identity is verified.
