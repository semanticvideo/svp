#!/usr/bin/env bash
set -euo pipefail

required_files=(
  "spec/SVP_v1_0_RC1.md"
  "spec/SVP_v1_0_RC1.pdf"
  "spec/SVP_v1_0_RC1.docx"
  "spec/SVP_v1_0_RC2.md"
  "spec/SVP_v1_0_RC2.pdf"
  "spec/SVP_v1_0_RC2.docx"
  "spec/review/SVP_v1_0_RC1_Review_Notes.md"
  "spec/review/SVP_Draft_v0_6_to_v1_0_RC1.diff"
  "spec/review/SVP_v1_0_RC2_Review_Notes.md"
  "spec/review/SVP_v1_0_RC1_to_RC2.diff"
  "spec/registries/validation-codes.json"
  "spec/registries/block-types.json"
  "spec/registries/equivalence-profile.json"
  "spec/registries/reference-model-set.json"
  "spec/registries/color-buckets.json"
  "spec/registries/color-spaces.json"
  "spec/registries/ocr-observation-types.json"
  "spec/registries/embedded-svpi-transport-profiles.json"
  "docs/svpi/Embedded_SVPI_Transport_ISO_BMFF_v1.md"
  "spec/schemas/index-manifest.schema.json"
  "spec/schemas/model-bundle.schema.json"
  "spec/schemas/model-lock.schema.json"
  "spec/schemas/signature-sidecar.schema.json"
  "spec/schemas/text-region.schema.json"
  "spec/schemas/text-observation.schema.json"
  "spec/schemas/numeric-value.schema.json"
  "spec/schemas/text-absence.schema.json"
  "spec/schemas/color-observation.schema.json"
  "spec/schemas/color-summary.schema.json"
  "spec/schemas/color-absence.schema.json"
  "spec/companion/SVP_Index_Manifest_v1_RC1.md"
  "spec/companion/SVP_Model_Bundle_v1_RC1.md"
  "spec/companion/SVP_Model_Bundle_v1_RC2.md"
  "spec/companion/SVP_Signature_Sidecar_v1_RC1.md"
  "releases/SVP_v1_0_RC1_Release_Package.zip"
  "releases/SVP_v1_0_RC2_Release_Package.zip"
  "docs/SVP_v1_0_RC2_Repo_Update_Handoff.md"
  "docs/SVP_Implementation_Phases_RC2_Update/README.md"
  "docs/SVP_Implementation_Phases_RC2_Update/00_PAUSE_PHASE_03_PLUS.md"
)

missing=0

for file in "${required_files[@]}"; do
  if [[ ! -f "$file" ]]; then
    echo "Missing: $file"
    missing=1
  else
    echo "OK: $file"
  fi
done

if [[ "$missing" -ne 0 ]]; then
  echo "One or more required files are missing."
  exit 1
fi

echo "All required SVP RC1 and RC2 files are present."
