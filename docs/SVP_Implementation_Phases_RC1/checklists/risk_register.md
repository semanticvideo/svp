# Risk Register

## Risk: Builder work starts too early

Mitigation: Build validator and fixtures first.

## Risk: Model runtime becomes Python-dependent

Mitigation: Use native model bundles and ONNX Runtime. No Python runtime for `svp build`.

## Risk: Validator reports vague failures

Mitigation: Use registry-backed validation codes and paths.

## Risk: SQLite equivalence causes false failures

Mitigation: Implement canonical logical row stream and source-layer equivalence rules from RC1.

## Risk: Vision pipeline over-promises

Mitigation: Start with conservative observations. Tracks do not need labels. Labels are non-core.

## Risk: First video trial uses too complex a source

Mitigation: Use one clean 20 to 30 second talking-head clip first.

## Risk: Agents modify shared contracts in parallel

Mitigation: Coordinate changes to CMake, core packages, and validation registry.
