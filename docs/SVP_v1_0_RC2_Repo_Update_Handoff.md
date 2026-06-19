# SVP v1.0 RC2 Repo Update Handoff

## Purpose

SVP v1.0 RC2 adds first-class OCR / visible-text observations and structured color observations to SVP Core. This update must be applied before continuing Phase 03+ implementation work.

RC2 is not a label-system expansion. Visible text and measured color distribution are Core observations.

Examples:

```text
Visible text "$12.99" is observation.
Numeric value 12.99 extracted from visible text is observation.
Shot color coverage orange = 0.70 is observation.
"This is an advertisement" is a label.
"This scene feels warm" is a label.
```

## Required repo updates

Copy the RC2 files into the repository as follows:

```text
spec/SVP_v1_0_RC2.md
spec/SVP_v1_0_RC2.pdf
spec/SVP_v1_0_RC2.docx
spec/review/SVP_v1_0_RC1_to_RC2.diff
spec/review/SVP_v1_0_RC2_Review_Notes.md
spec/registries/validation-codes.json
spec/registries/block-types.json
spec/registries/equivalence-profile.json
spec/registries/reference-model-set.json
spec/registries/color-buckets.json
spec/registries/color-spaces.json
spec/registries/ocr-observation-types.json
spec/schemas/text-region.schema.json
spec/schemas/text-observation.schema.json
spec/schemas/numeric-value.schema.json
spec/schemas/color-observation.schema.json
releases/SVP_v1_0_RC2_Release_Package.zip
docs/SVP_Implementation_Phases_RC2_Update/
```

Do not delete RC1 unless the human explicitly asks. RC1 remains useful as review history.

## Implementation pause rule

Do not continue Phase 03+, fixtures, media ingest, vision, relationships/index packaging, or first-video-trial work until OCR and color are represented in:

- validator required-path checks
- validator schema checks
- validation-code mapping
- fixture plan
- index/query plan
- builder roadmap
- first-video-trial acceptance criteria

## New required package paths

RC2 adds:

```text
text/
  text_regions.jsonl
  text_observations.jsonl
  numeric_values.jsonl
  text_absence.json

colors/
  color_observations.jsonl
  color_summary.json
  color_absence.json
```

These are Core sections for video packages.

## New registries

RC2 adds:

```text
color-buckets.json
color-spaces.json
ocr-observation-types.json
```

The validator must load these registries and use them to validate Core records.

## New schemas

RC2 adds:

```text
text-region.schema.json
text-observation.schema.json
numeric-value.schema.json
color-observation.schema.json
```

The validator must validate JSONL records against these schemas once deep package validation begins.

## Validator impact

New validation behavior must cover:

- missing `/text/`
- missing `/colors/`
- invalid text region records
- invalid text bounding boxes
- invalid text timing
- invalid text/frame/shot/scene/region/entity references
- invalid OCR confidence values
- invalid normalized text
- invalid numeric extraction
- invalid OCR provenance
- OCR/index mismatch
- invalid color observation records
- invalid color bucket IDs
- invalid color spaces
- invalid percentage totals
- invalid percentage bounds
- invalid dominant bucket references
- invalid sampling basis
- invalid color targets
- invalid color provenance
- color/index mismatch

## Builder impact

The builder roadmap must include separate stages for:

```text
ocr_text_detection
ocr_text_recognition
ocr_layout_classification
ocr_numeric_extraction
ocr_text_region_color_sampling
color_space_normalization
color_bucket_quantization
shot_color_summary
scene_color_summary
frame_color_summary
region_color_summary
entity_color_summary
text_region_foreground_background_color
text_index_integration
color_index_integration
```

Do not hardcode OCR or color into unrelated stages.

## First 30-second video target update

The first real video trial must now prove:

```bash
svp build samples/dom-30s.mov --out runs/first-video-trial/dom-30s.svp
svp validate runs/first-video-trial/dom-30s.svp --json
svp inspect runs/first-video-trial/dom-30s.svp
```

And the resulting package must include valid `/text/` and `/colors/` sections.

If the video contains visible text, it should produce text observations and index rows. If the video contains a safely parseable number, it should produce a numeric value record. Every scene and shot should have color observations.

## Definition of done for applying RC2

- RC2 spec files are present under `spec/`.
- New registries are present under `spec/registries/`.
- New schemas are present under `spec/schemas/`.
- RC2 release package is preserved under `releases/`.
- RC2 implementation update docs are present under `docs/`.
- Phase 03+ is paused until OCR/color implementation deltas are planned.
