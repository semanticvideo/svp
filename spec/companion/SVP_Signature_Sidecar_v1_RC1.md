# SVP Signature Sidecar v1 - RC1

`.svpsig` is an optional detached authenticity sidecar for `.svp` packages.

A reader MUST be able to process a valid `.svp` without a sidecar. A sidecar MUST NOT appear inside the `.svp` ZIP archive. Signature verification affects `authenticity_status` only and MUST NOT change SVP Core validity.

Signature findings are reported in the `authenticity` bucket of `provenance/validation.json` or external validation reports. They are excluded from default Core status and default validator exit-code computation.

A validator MAY provide an explicit signature-enforcement mode that exits non-zero for authenticity failures.
