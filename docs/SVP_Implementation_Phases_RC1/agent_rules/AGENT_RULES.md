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
