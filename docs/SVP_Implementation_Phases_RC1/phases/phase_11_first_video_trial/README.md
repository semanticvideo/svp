# Phase 11 - First 30-Second Video Trial

## Phase purpose

Run the actual trial that matters: one real 20 to 30 second video in, one `.svp` package out, validator and inspector working.

## Prerequisites

- Phase 10 complete.
- One simple 20 to 30 second source video is available under `samples/`.

## Primary outputs

```text
samples/dom-30s.mov or samples/dom-30s.mp4
out/dom-30s.svp
out/dom-30s.validation.json
out/dom-30s.inspect.txt
first-video-trial-report.md
```

## Work items

1. Add `samples/README.md` explaining that sample media may be local and not committed if large.
2. Run builder on the sample video.
3. Capture logs.
4. Run validator with JSON output.
5. Run inspector.
6. Save outputs under `out/` or `runs/first-video-trial/`.
7. Write a trial report including runtime, package size, warnings/errors, generated section counts, and screenshots or summaries if useful.
8. File issues for any invalid findings.

## Required commands

```bash
mkdir -p runs/first-video-trial
./build/tools/svp-builder/svp-builder build samples/dom-30s.mov --out runs/first-video-trial/dom-30s.svp 2>&1 | tee runs/first-video-trial/build.log
./build/tools/svp-validator/svp-validator validate runs/first-video-trial/dom-30s.svp --json > runs/first-video-trial/validation.json
./build/tools/svp-inspector/svp-inspector inspect runs/first-video-trial/dom-30s.svp > runs/first-video-trial/inspect.txt
```

## Definition of done

- A real `.svp` file is produced from a real video.
- Validator produces structured JSON.
- Inspector produces a useful summary.
- Trial report exists.
- Any invalid status is explained by specific actionable findings.

## Handoff report format

At the end of this phase, report:

```text
Phase: Phase 11 - First 30-Second Video Trial
Status: complete / blocked / partial
Files created:
Files modified:
Commands run:
Tests passed:
Tests failed:
Known issues:
Next recommended phase:
```
