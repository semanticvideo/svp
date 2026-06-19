# SVP Tools

The tools should be built in this order:

1. svp-validator
2. svp-reader
3. svp-inspector
4. svp-builder

Do not start with the builder.

## svp-validator

First priority.

Validates package structure, schemas, binary blocks, hashes, SQLite logical row streams, validation code usage, and equivalence behavior.

## svp-reader

Reads package contents and exposes structured access.

## svp-inspector

Human-readable package inspection CLI.

## svp-builder

Processes source media into SVP packages. This comes last.
