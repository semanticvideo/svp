# SVP Model Bundle v1 - RC1

SVP Model Bundles (`.svpmodel`) are ZIP64 packages used by the reference builder to distribute pinned, verified model artifacts without requiring a Python runtime.

## Canonical identity

All normative model references use canonical SVP-native `model_` IDs. Registry slugs are metadata only.

Example canonical model ID:

```text
model_depth_anything_v2_small
```

Example source metadata:

```json
{
  "source_registry": "github",
  "source_slug": "DepthAnything/Depth-Anything-V2"
}
```

## Bundle ID

`model_bundle_id` is derived as:

```text
<model_id>@<model_version>+blake3_<first_12_hex_of_bundle_blake3>
```

The full `bundle_blake3` field remains authoritative for verification.

## Required manifest fields

A conforming `model.svpmodel.json` contains `model_bundle_id`, canonical `model_id`, `model_version`, `bundle_blake3`, runtime, format, license, supported execution providers, files, input contract, output contract, preprocessor contract, and postprocessor contract.

`model.svpmodel.json` is authoritative for a single bundle. `model-lock.json` is authoritative for the selected set of bundles. If they disagree on identity or hashes, the builder rejects the model set.
