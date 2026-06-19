# SVP Packages / Libraries

Shared implementation modules should live here.

Suggested modules:

## svp-core

Core data types, IDs, timestamps, hash utilities, and common enums.

## svp-package

ZIP64 package reading/writing and required layout handling.

## svp-schema

JSON schema loading and validation.

## svp-blocks

SVPB binary block header parsing, validation, and hash verification.

## svp-index

SQLite index access and canonical logical row stream generation.

## svp-validation

Validation report model, validation code registry, status computation, equivalence reporting.
