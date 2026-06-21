# Agent Rules

These rules apply to every implementation agent.

## Read before coding

Before starting any phase, read:

```text
00_START_HERE.md
01_PARALLEL_WORK_MAP.md
03_TECHNICAL_DECISIONS.md
agent_rules/AGENT_RULES.md
```

Then read your assigned phase doc.

## Do not widen scope

Do not implement later phases unless explicitly assigned.

Do not start the full builder while assigned to validator work.

Do not change the spec unless the task explicitly asks for a spec patch.

## Report every command

At the end of work, report:

```text
Files created
Files modified
Commands run
Tests passed
Tests failed
Known issues
Next recommended task
```

## Keep validation codes disciplined

Use registered validation codes from:

```text
spec/registries/validation-codes.json
```

If a needed code does not exist, use a temporary internal code prefixed with:

```text
X_VALIDATOR_
```

Then document that the registry needs review.

## Build before claiming done

If a phase includes code, it is not done unless the relevant build command was run.

## Prefer small commits

Each phase should produce clean commits. Avoid one giant all-purpose commit.

## No magic-number fixes

Do not solve a bug by adding a literal number, timestamp, filename, expected text value, sample count, threshold, or crop coordinate that only works for the current sample.

If a number controls correctness, sampling, coverage, timing, validation, runtime limits, confidence, or model identity, it needs:

- a clear name,
- a single owner,
- a reason,
- tests for the general behavior,
- and provenance or diagnostics when it limits what SVP can observe.

For temporal media, define coverage. A fixed number of frames is not enough. State the sample cadence, maximum gap, cap behavior, and what duration of event may be missed.

Reviewers should flag unexplained literals or sample-specific behavior as findings.

## Protect shared files

Coordinate changes to:

```text
CMakeLists.txt
vcpkg.json
packages/svp-core
packages/svp-validation
spec/registries/validation-codes.json
```

## Do not hide failures

If a dependency fails to install or a test fails, report it. Do not paper over failures with placeholder success messages.
