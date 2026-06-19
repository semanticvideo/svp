# Phase 09 Delta: OCR and Color Builder Stages

Add independent builder stages for OCR text detection, OCR recognition, OCR layout classification, numeric extraction, color quantization, shot/scene/frame color summaries, region/entity color summaries, text-region color summaries, and text-region foreground/background color sampling.

Builder output must populate RC2 `/text/` and `/colors/` Core observations, not label records:

- detected text regions with stable region targets
- recognized visible text
- normalized text
- safely parseable numeric values extracted from visible text
- honest text absence when no visible text is present
- registry-backed color bucket observations
- shot, scene, frame/keyframe, region, entity, and text-region color bucket percentages
- foreground/background color summaries for text regions
- honest color absence when color coverage cannot be measured

Color processing should be classical and deterministic first. OCR may be model-backed through SVP Model Bundles.
