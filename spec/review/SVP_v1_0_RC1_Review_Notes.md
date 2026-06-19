# SVP v1.0 RC1 Review Notes

RC1 is a release-candidate clarification pass over Draft 0.6. It preserves strict SVP Core and fixes four implementation blockers found in final review.

## Accepted RC blockers

1. **Defined `index/index_manifest.json`**
   - Added Section 17.6.
   - Defined `logical_rows_blake3` as the BLAKE3 of the Section 17.5 canonical SQLite logical row stream.
   - Added `index-manifest.schema.json` to the required schema list.
   - Added `ERR_CORE_INDEX_MANIFEST_INVALID`.

2. **Separated authenticity from Core status**
   - Signature mismatch and unreadable signature now report through the `authenticity` bucket.
   - `ERR_SIGNATURE_*` codes do not affect Core `status` or default validator exit code.
   - Explicit signature-enforcement mode may still exit non-zero as an invocation policy.

3. **Unified model ID namespace**
   - Canonical SVP `model_` IDs are now required everywhere normative: embedding sets, provenance, model bundles, locks, DAG tasks, cache keys, and equivalence checks.
   - Registry slugs are source metadata only.

4. **Completed model bundle manifest contract**
   - Added `model_bundle_id` derivation.
   - Expanded the example to include `preprocessor_contract`, `postprocessor_contract`, `supported_execution_providers`, and bundle/file hashes.
   - Defined authority relationship between `model.svpmodel.json` and `model-lock.json`.

## RC posture

After RC1, changes should be limited to contradiction fixes, validator-blocking ambiguity fixes, schema defects, fixture-driven conformance corrections, security footgun fixes, implementation impossibility fixes, and editorial clarity.
