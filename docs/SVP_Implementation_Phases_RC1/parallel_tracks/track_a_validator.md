# Track A - Validator

## Owns

```text
phase_02_validator_core
phase_03_validator_deep_package_checks
part of phase_04_fixture_lab
```

## Goal

Create the tool that defines whether an `.svp` package is valid.

## Must produce

```text
svp-validator validate package.svp
svp-validator validate package.svp --json
svp-validator validate --equivalent a.svp b.svp
```

`--equivalent` can come after core validation, but the architecture must not block it.

## Key libraries

```text
packages/svp-validation
packages/svp-package
packages/svp-blocks
packages/svp-index
```

## Stop condition

The validator can validate generated fixtures and report all failures with machine-readable codes.
