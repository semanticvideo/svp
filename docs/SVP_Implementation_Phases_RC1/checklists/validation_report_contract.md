# Validation Report Contract

The validator should emit this shape for `--json` output:

```json
{
  "schema_version": "svp-validation-report-v1",
  "validator": {
    "name": "svp-validator",
    "version": "0.1.0"
  },
  "status": "valid",
  "core_status": "valid",
  "authenticity_status": "not_checked",
  "errors": [],
  "warnings": [],
  "infos": [],
  "authenticity": []
}
```

## Status values

```text
valid
valid_with_warnings
invalid
unreadable
```

## Exit codes

```text
0 = valid or valid_with_warnings
1 = invalid Core package
2 = unreadable input or validator runtime error
```

## Authenticity rule

Signature findings must not affect Core status by default.

If signature enforcement is added later, it must be behind an explicit flag such as:

```bash
svp-validator validate package.svp --require-valid-signature
```
