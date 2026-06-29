# SVPI Specification Drafting Brief

This document is a self-contained brief for an agent that does **not** have access to the SVP repository. Its job is to draft a first-pass specification for **SVPI**, the Semantic Video Package Interlace sidecar format.

The agent should produce a specification draft, not implementation code.

## Assignment For The Spec-Drafting Agent

Write a detailed Markdown specification draft for **SVPI v0.1**, a sidecar/interlace format for semantic video observations.

The draft should be suitable for later critique and refinement by the SVP maintainer. It should define purpose, workflows, file layout, media binding, validation rules, recombination rules, extraction rules, and future camera-native extension points.

The draft must be concrete enough that a later builder can implement a prototype, but it must not overclaim final standard status.

## What SVP Is

SVP stands for **Semantic Video Package**.

An `.svp` file is the primary packaged representation of a video plus semantic observations. It is a single-file package used for local-first video understanding, validation, querying, traversal, and agent navigation.

SVP packages are currently ZIP64-based package files with:

- a required `mimetype` entry
- a required `manifest.json`
- embedded source media under `media/original/`
- transcript artifacts
- timeline artifacts
- entity artifacts
- spatial/depth/mask artifacts
- OCR/visible-text artifacts
- color observations
- relationships
- embeddings
- SQLite index files
- provenance records
- validation output

An SVP file is intended to be self-contained for interchange. It can be opened, validated, queried, and traversed without requiring a sibling media file.

### Current SVP Core Sections

The current SVP package contract requires these root sections or files:

```text
mimetype
manifest.json
media/
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

The package also allows a future or optional `labels/` section, but labels are not the core semantic observation model.

### Important SVP Layers

The actual file set evolves, but the spec-drafting agent should understand these current concepts:

- `manifest.json`: package identity, media/timing metadata, canonical analysis raster, package metadata.
- `media/original/source_000.*`: original source media copied into the package.
- `transcript/words.jsonl`: word-level transcript records with timestamps and speaker IDs.
- `transcript/speakers.jsonl`: anonymous speaker summaries.
- `transcript/speaker_segments.jsonl`: diarization segments.
- `timeline/frames.jsonl`: sampled/decoded frame records.
- `timeline/shots.jsonl`: shot intervals.
- `timeline/scenes.jsonl`: scene intervals.
- `entities/entities.jsonl`: visual entity records.
- `entities/entity_tracks.jsonl`: entity track records.
- `spatial/regions.jsonl`: visual/spatial region records.
- `spatial/masks.index.jsonl`: mask block index records.
- `spatial/masks.blocks.svpmz`: binary mask block stream.
- `spatial/depth.index.jsonl`: depth block index records.
- `spatial/depth.blocks.svpdz`: binary depth block stream.
- `text/text_regions.jsonl`: detected visible text regions.
- `text/text_observations.jsonl`: recognized visible text.
- `text/numeric_values.jsonl`: numeric values extracted from visible text.
- `text/text_absence.json`: honest absence/blocker record when text is absent or OCR cannot run.
- `text/evidence_crops/`: bounded OCR evidence crops.
- `colors/color_observations.jsonl`: measured color observations.
- `colors/color_summary.json`: aggregate color summaries.
- `colors/color_absence.json`: honest absence/blocker record when color output cannot exist.
- `relationships/relationships.jsonl`: support and semantic relationships between objects.
- `relationships/relationship_provenance.json`: relationship-generation provenance and counts.
- `embeddings/embeddings.index.jsonl`: embedding index records.
- `embeddings/embeddings.blocks.svpez`: binary embedding block stream.
- `index/index.sqlite`: required SQLite index for fast local query.
- `index/index_manifest.json`: index schema/logical-row integrity metadata.
- `provenance/processors.jsonl`: processor records.
- `provenance/model_hashes.jsonl`: model provenance where applicable.
- `provenance/validation.json`: machine-readable validation result.

### Current SVP Query/Traversal Model

SVP can be queried at the single-package level. The current model supports:

- transcript and word lookup
- speaker lookup
- OCR lookup
- color lookup
- relationship listing
- graph traversal
- shortest paths
- context around objects
- graph health diagnostics

This matters because SVPI should reuse the same observation model and object IDs wherever possible.

## What SVPI Is

SVPI stands for **Semantic Video Package Interlace**.

An `.svpi` file is a semantic sidecar/interlace file that stores observations about a video **without containing the primary media itself**.

An SVPI weaves semantic observations together with existing source media. It can be created during or after video production.

SVPI is not merely "some metadata about a video." It is bound to a specific source media identity and, in future camera-native cases, optionally to capture hardware/device identity.

### Core Principle

SVPI **does not replace SVP**.

SVP remains the primary packaged representation.

SVPI is an additional sidecar representation that shares the same observation model.

```text
SVP  = packaged media + semantic observations
SVPI = media-bound semantic observations without packaged media
```

## First Scope For SVPI v0.1

The first SVPI spec draft should focus on source-video-derived sidecars.

Do **not** make camera-native hardware capture required in v0.1. Reserve it clearly for future extension.

The first implementation target is:

```text
source video -> builder -> .svpi
source video + .svpi -> builder/packager -> .svp
.svp -> extractor -> source media + .svpi
```

Camera-native SVPI should be described as a future-compatible design target, not as required v0.1 implementation.

## Required Conceptual Rules

The spec draft must preserve these rules.

### Rule 1: SVPI Is A Sidecar, Not A Complete Video Package

An SVPI does not contain the primary video/media bytes.

It may contain small evidence artifacts if the spec chooses to allow them, such as thumbnails, OCR crops, summaries, or compact derived data, but it must not rely on those as replacement media.

If the source media is missing, an SVPI may be inspectable, but it cannot be recombined into a valid SVP until matching source media is provided.

### Rule 2: SVPI Is Bound To Media Identity, Not Absolute Path

An SVPI must not be valid merely because it points to:

```text
/Volumes/DriveA/project/video.mov
```

Absolute paths are only hints.

The binding truth must be media identity:

- content hash or chunked content hash
- media size
- duration
- stream fingerprints
- container/codec information
- optional capture metadata
- optional original filename
- optional relative path hints
- optional volume/path hints

Moving these files should work:

```text
video.mov
video.svpi
```

from one drive to another, as long as the media identity still verifies.

### Rule 3: Path Hints Are Convenience, Hashes/Fingerprints Are Truth

The spec should distinguish:

- required media identity fields
- optional location hints
- optional library/catalog hints

Recommended language:

```text
Path = hint.
Media identity = truth.
```

### Rule 4: SVPI Can Be Made From Source Video

A builder should be able to process a normal media file and emit:

```text
video.mov
video.svpi
```

The SVPI may contain:

- transcript observations
- OCR observations
- color observations
- timeline observations
- entities/tracks
- spatial/depth/mask observations
- embeddings
- relationships
- provenance
- index data
- validation data

The source video remains untouched.

### Rule 5: SVPI Can Be Extracted From SVP

A tool should be able to take:

```text
video.svp
```

and produce:

```text
video.mov
video.svpi
```

The extracted SVPI must be media-bound to the extracted source media.

### Rule 6: SVP Can Be Recombined From Source Video + SVPI

A tool should be able to take:

```text
video.mov
video.svpi
```

verify that the media matches the SVPI binding, and produce:

```text
video.svp
```

If the media does not match, recombination must fail or produce an explicitly invalid/unbound result. It must not silently pair an SVPI with unrelated media.

### Rule 7: Existing SVP Workflow Remains Valid

This workflow remains primary and supported:

```text
Video -> Builder -> SVP
```

SVPI adds new workflows. It does not remove or deprecate direct SVP building.

### Rule 8: SVPI Shares The Observation Model With SVP

SVPI should reuse SVP's conceptual layers:

- transcript
- timeline
- entities
- spatial
- text/OCR
- colors
- relationships
- embeddings
- index
- provenance
- validation

The difference is storage/transport and media binding, not semantic meaning.

### Rule 9: Camera-Native SVPI Is Future Scope

Future capture devices may create SVPI directly while recording.

Examples:

- iPhone app records `video.mov` and `video.svpi`
- drone records media and telemetry SVPI
- cinema camera records lens/timecode/calibration SVPI

Camera-generated SVPI may include capture-time observations:

- lens used
- focal length
- exposure duration
- ISO
- white balance
- frame timing
- orientation
- device identity
- depth streams
- motion sensors
- capture timestamps
- calibration data
- device-specific telemetry

These are not AI-derived observations. They are direct capture-time observations and may be more authoritative than later inference.

For v0.1, this must be future-reserved, not required.

## Required Workflows To Define

The spec draft must define these workflows.

### Workflow A: Existing SVP Build

```text
Video -> Builder -> SVP
```

Produces a complete packaged `.svp`.

### Workflow B: Sidecar Build

```text
Video -> Builder -> SVPI
```

Produces:

```text
video.mov
video.svpi
```

The original video is untouched.

### Workflow C: Recombine

```text
Video + SVPI -> Builder/Packager -> SVP
```

Produces a packaged `.svp` only after media binding verification passes.

### Workflow D: Extract

```text
SVP -> Extract -> Video + SVPI
```

Produces source media plus a media-bound `.svpi`.

### Workflow E: Future Camera-Native Capture

```text
Capture Device -> video.mov + video.svpi
```

The camera/capture application writes source media and SVPI at capture time.

For v0.1, describe but reserve this workflow.

## CLI Shape To Consider

The spec draft should propose CLI names, but mark them as non-final.

Possible commands:

```bash
svp build video.mov --out video.svp

svp interlace create video.mov --out video.svpi

svp interlace package video.mov video.svpi --out video.svp

svp interlace extract video.svp --out-dir ./out

svp interlace validate video.svpi --media video.mov

svp interlace inspect video.svpi
```

The agent may propose better names, but must preserve the workflow meanings.

## Media Binding Design Requirements

The spec draft should define a `media_binding` concept.

It should include required and optional fields.

Suggested required fields:

```json
{
  "media_binding": {
    "binding_id": "media_binding_000001",
    "media_role": "primary_source",
    "media_id": "media_000001",
    "duration_us": 123456789,
    "size_bytes": 987654321,
    "container_format": "mov",
    "stream_fingerprints": [],
    "content_blake3": "... or null if chunked proof is used",
    "chunk_hashes": [],
    "verification_policy": "full_hash_required | chunk_hash_allowed | metadata_only_not_trusted"
  }
}
```

Suggested optional hint fields:

```json
{
  "location_hints": {
    "original_filename": "video.mov",
    "relative_path": "./video.mov",
    "original_absolute_path": "/Volumes/DriveA/video.mov",
    "volume_hint": "DriveA",
    "last_seen_utc": "..."
  }
}
```

The spec should state clearly:

- hints are not proof
- absolute path mismatch is not a validation failure by itself
- media identity mismatch is a binding failure
- recombination requires binding verification

## Container / File Layout Questions

The agent should propose and discuss options, then recommend one for v0.1.

Possible SVPI container choices:

1. ZIP64 sidecar with similar layout to SVP, but no `media/original/`
2. directory/bundle representation
3. single JSON/JSONL manifest with external block files
4. SQLite-centric sidecar

The recommended v0.1 direction should likely be:

```text
ZIP64 sidecar with SVP-like layout minus primary media bytes.
```

Reasons:

- aligns with SVP package layout
- easy extraction/recombination
- supports JSON/JSONL plus binary blocks
- can carry index/provenance/validation
- can remain a single portable file

But the agent should still document tradeoffs.

## Candidate SVPI Layout

The spec draft should propose a candidate layout.

Example:

```text
mimetype
svpi_manifest.json
media_binding.json
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
validation/
extensions/
```

Important differences from SVP:

- no required `media/original/` primary media bytes
- required `media_binding.json` or equivalent manifest section
- optional `media/derived/` or `evidence/` may be discussed, but must not replace primary media
- `extensions/` may be reserved for provider/camera-specific observations

The draft should decide whether SVPI uses:

- `manifest.json` with `"format": "svpi"`
- or `svpi_manifest.json`

The draft should include pros and cons.

## Observation Reuse Rules

SVPI should preserve SVP object IDs and relationships when extracted from an SVP.

When generated independently from source media, it should generate IDs using the same deterministic principles as SVP.

The spec draft should cover:

- stable IDs
- timestamp basis
- source media timeline
- relationship references
- provenance references
- index rows
- equivalence rules
- validation reports

## Provenance Rules

Every SVPI observation must have provenance.

The draft should distinguish:

- capture-time provenance
- builder-derived provenance
- extracted-from-SVP provenance
- recombined-into-SVP provenance

For extracted SVPI, provenance should identify:

- source SVP package identity
- extraction tool/version
- original package hash if available
- extracted media identity
- extracted observation identity

For recombined SVP, provenance should identify:

- input source media identity
- input SVPI identity/hash
- media binding verification result
- packager tool/version

## Validation Rules

The spec draft should define validation modes.

Suggested modes:

```text
structure validation:
  Is this a well-formed SVPI?

binding validation:
  Does this SVPI match the supplied source media?

recombination validation:
  Can this SVPI and media produce a valid SVP?

equivalence validation:
  Is an extracted/recombined SVP equivalent to the original within allowed rules?
```

Important rule:

An SVPI may be structurally valid while unbound because the source media is not currently available.

But an unbound SVPI cannot be recombined into a valid SVP.

The spec should define states such as:

```text
valid_bound
valid_unbound
invalid_structure
binding_mismatch
unsupported_version
```

or propose better names.

## Authority Hierarchy

The spec draft must include the concept of observation authority.

Proposed authority levels:

```text
capture_time
builder_derived
package_extracted
user_imported
provider_extension
```

Capture-time observations can be more authoritative than later estimates.

Example:

- camera-reported focal length should generally outrank inferred focal length
- camera-reported orientation should generally outrank post-derived orientation

But the system must not silently discard derived observations. It should preserve both and explain provenance.

## Provider Extensions

The draft should define a safe extension area.

Examples:

```text
extensions/apple_iphone/
extensions/drone/
extensions/cinema_camera/
```

or a manifest-driven namespace model.

Extension rules:

- unknown optional extensions must not break core SVPI validation
- required extensions must be declared
- provider-specific observations must carry provenance
- extension records must not override core records silently

## Authenticity Potential

The draft should explain how SVPI may relate to authenticity later.

Possible chain:

```text
capture:
  video.mov
  video.svpi

builder:
  enriched video.svpi

packager:
  video.svp

signer:
  video.svp + video.svpsig
```

SVPI should not require signatures in v0.1, but should leave space for:

- capture-device attestation
- signed media binding
- C2PA-compatible references
- package signatures

## Relationship To Existing `.svpsig`

SVP already has a concept of detached `.svpsig` authenticity sidecars.

The SVPI draft must not confuse `.svpi` with `.svpsig`.

Difference:

```text
.svpi   semantic observation sidecar bound to source media
.svpsig detached authenticity/signature sidecar for package bytes or canonical signature payload
```

They may work together later, but they are not the same format.

## Relationship To Future Library Search

SVPI should be designed before library/catalog search.

A future library index may scan:

```text
video_a.svp
video_b.mov + video_b.svpi
video_c.mp4 + video_c.svpi
```

and build a cross-package/cross-sidecar catalog.

The library index is a software/product layer, not a core format requirement.

The SVPI spec should ensure library search has enough stable identity fields to catalog sidecars reliably.

## Open Questions The Draft Should Surface

The agent should include an "Open Questions" section.

At minimum, address:

1. Should SVPI use `manifest.json` or `svpi_manifest.json`?
2. Should SVPI be ZIP64 like SVP?
3. Should `index/index.sqlite` be required in SVPI v0.1 or optional?
4. Should evidence crops be stored inside SVPI?
5. Should depth/mask/embedding block streams be allowed in SVPI?
6. What is the minimum required SVPI for a sidecar generated from video?
7. What validation state should represent a structurally valid SVPI whose media is missing?
8. How strong must media binding be for normal local workflows?
9. Should full media BLAKE3 be mandatory, or can chunked fingerprints satisfy v0.1?
10. How should extracted SVPI preserve original SVP package identity?
11. How should camera-native fields be reserved without over-designing hardware capture now?
12. How should provider extensions declare whether they are optional or required?

## Expected Output From The Spec-Drafting Agent

The agent should output:

1. A Markdown spec draft titled:

   ```text
   SVPI v0.1 Draft Specification
   ```

2. A concise design summary.

3. Normative language where useful:

   - MUST
   - MUST NOT
   - SHOULD
   - MAY

4. A proposed file layout.

5. JSON examples for:

   - SVPI manifest
   - media binding
   - provenance record
   - validation state

6. CLI workflow examples.

7. Validation rules.

8. Recombination rules.

9. Extraction rules.

10. Future camera-native extension section.

11. Open questions.

12. A "not in v0.1" section.

## Things The Agent Must Not Do

- Do not claim SVPI replaces SVP.
- Do not require camera-native hardware capture in v0.1.
- Do not make absolute paths authoritative.
- Do not allow recombination with unrelated media.
- Do not redefine SVP observations as labels.
- Do not merge `.svpi` and `.svpsig` concepts.
- Do not require a library/catalog index as part of SVPI Core.
- Do not design a cloud service.
- Do not assume source media can always be modified.
- Do not silently discard provenance when extracting or recombining.

## Tone And Intent

The spec should feel like an emerging open technical standard:

- clear
- practical
- local-first
- provenance-aware
- media-format-friendly
- compatible with existing workflows
- cautious about authenticity claims
- explicit about future camera-native potential

The purpose is not to finish the final standard in one pass. The purpose is to produce a strong draft that can be reviewed, criticized, and refined before implementation.
