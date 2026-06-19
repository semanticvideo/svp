# SVP v1.0 RC2 Review Notes

## Purpose

RC2 incorporates first-class OCR / visible text observations and structured color observations into SVP Core. This is a scope-expanding release-candidate revision, but it is not a weakening revision. Existing RC1 Core requirements remain required.

## Accepted architectural decision

OCR and color are Core observations for SVP v1.0 RC2.

This means `/text/` and `/colors/` are required package sections for video packages, not optional label sections. A visible string and a measured color distribution are treated as observations. Interpreting the string or color as an ad, warning, sunset, brand, mood, or material is still a label or higher-level interpretation.

## Main changes

1. Added required `/text/` package section:
   - `text_regions.jsonl`
   - `text_observations.jsonl`
   - `numeric_values.jsonl`
   - `text_absence.json`

2. Added required `/colors/` package section:
   - `color_observations.jsonl`
   - `color_summary.json`
   - `color_absence.json`

3. Added OCR schema artifacts:
   - `text-region.schema.json`
   - `text-observation.schema.json`
   - `numeric-value.schema.json`

4. Added color schema artifacts:
   - `color-observation.schema.json`

5. Added registries:
   - `color-buckets.json`
   - `color-spaces.json`
   - `ocr-observation-types.json`

6. Updated validation-code registry with OCR and color errors.

7. Updated equivalence profile with OCR and color equivalence behavior.

8. Updated reference model set with OCR detector/recognizer/layout analyzer model IDs.

9. Added implementation pause guidance: do not continue Phase 03+ until OCR and color are represented in validator, fixtures, index, builder roadmap, and first-video-trial acceptance criteria.

## Key principle

```text
Text visible in the video is observation.
Numeric text visible in the video is observation.
Measured pixel color distribution is observation.
Interpreting what those observations mean is not Core.
```

## Implementation impact

Validator work must expand before deep package validation continues. The first implementation track should add schema and registry awareness for `/text/` and `/colors/`, then update fixtures and index checks. Builder work should not proceed into media ingest/vision/index packaging until OCR/color records are represented in the roadmap and acceptance criteria.
