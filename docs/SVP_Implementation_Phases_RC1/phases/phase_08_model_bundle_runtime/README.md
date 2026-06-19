# Phase 08 - Native Model Bundle Runtime

## Phase purpose

Implement the native model-bundle machinery needed by audio, depth, masks, and embeddings. This keeps the builder free from Python runtime dependency.

## Prerequisites

- Phase 01 complete.
- RC1 model bundle docs and schemas exist under `spec/`.

## Primary outputs

```text
packages/svp-models
model bundle manifest parser
model-lock parser
BLAKE3 model verification
ONNX Runtime session loader
model cache directory
svp models verify/list/install commands, if CLI scope allows
```

## Work items

1. Add `packages/svp-models`.
2. Parse `model.svpmodel.json` according to schema.
3. Parse `model-lock.json`.
4. Verify bundle file hashes using BLAKE3.
5. Enforce canonical `model_...` IDs only for normative references.
6. Treat registry slugs as metadata only.
7. Implement ONNX Runtime session wrapper with execution provider selection.
8. Add model cache path discovery.
9. Add clear error messages for missing models.
10. Add `svp models verify` if unified CLI exists, or a temporary `svp-models-tool verify`.

## Required commands

```bash
cmake --build build
./build/tools/svp-builder/svp-builder models verify --model-set spec/registries/reference-model-set.json
```

## Definition of done

- Model bundle manifests validate.
- Hash checks work.
- ONNX Runtime session can be created for at least one tiny test model or placeholder model.
- Missing model errors are clear and non-crashing.
- No Python runtime is required.

## Handoff report format

At the end of this phase, report:

```text
Phase: Phase 08 - Native Model Bundle Runtime
Status: complete / blocked / partial
Files created:
Files modified:
Commands run:
Tests passed:
Tests failed:
Known issues:
Next recommended phase:
```
