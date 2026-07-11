# Phase 12 - Hardening, RC Feedback, and Next Standard Pass

## Phase purpose

Use implementation results to identify true spec bugs, validator gaps, builder issues, and remaining reference-implementation gaps. This phase now starts after the repository has produced validator-clean real-video packages with OCR, color, depth, embeddings, evidence crops, visual entity tracks, spatial regions, mask block streams, and whisper.cpp ASR. Phase 12 decides what must be completed for full RC2/reference conformance versus what belongs in post-RC2 quality hardening.

## Prerequisites

- Phase 11 complete.
- First video trial report exists.

## Primary outputs

```text
implementation gap list
validator gap list
fixture gap list
RC2 spec issue list
post-PR64 completion backlog
parallelization plan
v1.0 final or post-RC2 clarification recommendation
```

## Work items

1. Review validator results, package contents, and human-baseline findings from the real video trials.
2. Separate milestone-valid package behavior from full RC2/reference-builder conformance.
3. Maintain the post-PR64 backlog in stackable lanes:
   - Query and inspection usability.
   - Speaker diarization and speaker embeddings.
   - Deterministic visual entity identity fixtures.
   - Mask/depth-backed spatial relationships.
   - Relationship graph traversal.
   - Transcript chunk embeddings and vision embeddings.
   - Strict validator/spec enforcement.
   - Canonical model-bundle BLAKE3 verification.
   - OCR/scene/ASR quality hardening.
   - Installable SVP agent skill and developer guide.
4. Identify which lanes can run in parallel and which touch shared contract zones.
5. Add fixture coverage for each discovered implementation bug or validator gap.
6. Add validation codes only if truly needed.
7. Document any RC2 spec contradiction or ambiguity.
8. Decide whether a post-RC2 clarification is required or RC2 can proceed toward v1.0 final.
9. Prepare a concise human-readable project status report.

## Required commands

```bash
git status
./build/tools/svp-validator/svp-validator validate runs/first-video-trial/dom-30s.svp --json
```

## Definition of done

- Trial findings are triaged.
- Remaining full-spec gaps are sorted into stackable lanes.
- Parallel-safe and shared-contract work are clearly separated.
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
