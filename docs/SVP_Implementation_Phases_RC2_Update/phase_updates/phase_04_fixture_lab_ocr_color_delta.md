# Phase 04 Delta: OCR and Color Fixtures

Add valid fixtures for visible text, normalized visible text, numeric text, text absence, measured color coverage, and color absence. Visible text, numeric values extracted from visible text, and measured color coverage are Core observations, not labels.

Required fixture cases:

- no visible text with valid `text_absence.json`
- visible UI text with `text_regions.jsonl` and `text_observations.jsonl`
- safely parseable numeric text with `numeric_values.jsonl`
- subtitle or caption text
- chart or dashboard number
- red foreground text with text-region foreground/background colors
- orange scene with searchable scene color coverage
- yellow shot with searchable shot color coverage
- green number with numeric extraction and text-region colors
- uniform black frame
- uniform white frame
- no measurable color case with valid `color_absence.json`
- invalid color bucket totals
- invalid color bucket ID
- invalid OCR/text reference

Fixture docs must state the expected validator status for each case and identify which `/text/` and `/colors/` files are expected to exist.
