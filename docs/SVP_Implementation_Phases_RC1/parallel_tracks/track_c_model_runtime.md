# Track C - Model Runtime

## Owns

```text
phase_08_model_bundle_runtime
model-related portions of phase_07, phase_09, phase_10
```

## Goal

Make local model execution boring and native.

## Non-negotiables

- No Python runtime for normal builder operation.
- All model bundles are content-addressed.
- Canonical `model_...` IDs are used in cache keys, provenance, embedding sets, and bundle manifests.
- Registry slugs are metadata only.

## Stop condition

At least one ONNX model bundle can be verified, loaded, and run through the native runtime wrapper.
