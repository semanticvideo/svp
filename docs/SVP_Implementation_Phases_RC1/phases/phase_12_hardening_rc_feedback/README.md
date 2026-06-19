# Phase 12 - Hardening, RC Feedback, and Next Standard Pass

## Phase purpose

Use implementation results to identify true spec bugs, validator gaps, and builder issues. This is where RC2 either becomes a v1.0 final candidate or receives a later clarification pass.

## Prerequisites

- Phase 11 complete.
- First video trial report exists.

## Primary outputs

```text
implementation gap list
validator gap list
fixture gap list
RC2 spec issue list
v1.0 final or post-RC2 clarification recommendation
```

## Work items

1. Review validator failures and warnings from the first video trial.
2. Separate implementation bugs from spec ambiguities.
3. Add fixture coverage for each discovered bug.
4. Add validation codes only if truly needed.
5. Document any RC2 spec contradiction or ambiguity.
6. Decide whether a post-RC2 clarification is required or RC2 can proceed toward v1.0 final.
7. Prepare a concise human-readable project status report.

## Required commands

```bash
git status
./build/tools/svp-validator/svp-validator validate runs/first-video-trial/dom-30s.svp --json
```

## Definition of done

- Trial findings are triaged.
- New issues are filed or documented.
- No architecture creep is introduced.
- Clear recommendation exists: fix implementation, draft a clarification, or finalize v1.0.

## Handoff report format

At the end of this phase, report:

```text
Phase: Phase 12 - Hardening, RC Feedback, and Next Standard Pass
Status: complete / blocked / partial
Files created:
Files modified:
Commands run:
Tests passed:
Tests failed:
Known issues:
Next recommended phase:
```
