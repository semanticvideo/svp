# Pause Phase 03+ Until RC2 OCR/Color Integration

## Rule

Do not continue Phase 03+, fixture lab, media ingest, vision pipeline, relationships/index packaging, or first-video-trial work until OCR and structured color observations are represented in the implementation plan.

## Why

RC2 makes `/text/` and `/colors/` Core package sections. A validator, builder, fixture set, index model, and inspector that do not understand OCR and color will immediately drift from the RC2 spec.

## Minimum unblock checklist

- Validator can detect required `/text/` and `/colors/` paths.
- Validator can load OCR/color schemas and registries.
- Fixture plan includes visible text, numeric text, and color coverage cases.
- Index schema includes text and color query tables.
- Builder roadmap has separate OCR and color stages.
- First 30-second video trial acceptance criteria include visible text/color checks.
