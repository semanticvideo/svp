# Phase 00 - Repo Baseline and RC1 Lock

## Phase purpose

Lock the repository foundation before implementation starts. This prevents agents from coding on top of an uncommitted or drifting spec layout.

## Prerequisites

- Existing repo setup handoff has been completed.
- `scripts/verify-spec-files.sh` exists and passes.

## Primary outputs

```text
A clean git commit containing the RC1 spec foundation.
A verified repo status baseline.
```

## Work items

1. Run `scripts/verify-spec-files.sh`.
2. Run `git status`.
3. Inspect whether anything unexpected is present in `.tmp/` or root.
4. Commit the setup foundation.
5. Tag locally if desired as `svp-v1.0-rc1-spec-import`.

## Required commands

```bash
cd /Users/domesposito/Projects/svp
scripts/verify-spec-files.sh
git status
git add .
git commit -m "chore(spec): add SVP v1.0 RC1 repository foundation"
```

## Definition of done

- Verification script passes.
- RC1 package is preserved under `releases/`.
- Git working tree is clean after commit.
- No implementation code has started in this phase.

## Handoff report format

At the end of this phase, report:

```text
Phase: Phase 00 - Repo Baseline and RC1 Lock
Status: complete / blocked / partial
Files created:
Files modified:
Commands run:
Tests passed:
Tests failed:
Known issues:
Next recommended phase:
```
