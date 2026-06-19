# Phase 02 Delta: Validator Core for OCR and Color

Add awareness of required RC2 paths: `/text/` and `/colors/`. Load new registries: `color-buckets.json`, `color-spaces.json`, `ocr-observation-types.json`. Load new schemas: `text-region`, `text-observation`, `numeric-value`, `color-observation`.

Do not implement full OCR/color semantic validation here. Phase 02 only confirms the validator knows these sections exist and reports missing sections with registered codes.
