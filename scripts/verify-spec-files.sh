#!/usr/bin/env bash
set -euo pipefail

required_files=(
  "spec/SVP_v1_0_RC1.md"
  "spec/SVP_v1_0_RC1.pdf"
  "spec/SVP_v1_0_RC1.docx"
  "spec/review/SVP_v1_0_RC1_Review_Notes.md"
  "spec/review/SVP_Draft_v0_6_to_v1_0_RC1.diff"
  "spec/registries/validation-codes.json"
  "spec/registries/block-types.json"
  "spec/registries/equivalence-profile.json"
  "spec/registries/reference-model-set.json"
  "spec/schemas/index-manifest.schema.json"
  "spec/schemas/model-bundle.schema.json"
  "spec/schemas/model-lock.schema.json"
  "spec/schemas/signature-sidecar.schema.json"
  "spec/companion/SVP_Index_Manifest_v1_RC1.md"
  "spec/companion/SVP_Model_Bundle_v1_RC1.md"
  "spec/companion/SVP_Signature_Sidecar_v1_RC1.md"
  "releases/SVP_v1_0_RC1_Release_Package.zip"
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

echo "All required SVP RC1 files are present."
