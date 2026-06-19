# Agent Definition of Done

A phase is not complete unless the agent reports:

```text
Files created
Files modified
Commands run
Tests passed
Tests failed
Known issues
Next recommended task
```

For code phases:

- The code must build.
- Relevant commands must be run.
- Failures must be shown, not hidden.

For fixture phases:

- Fixtures must be reproducible.
- Expected validator statuses must be documented.
- RC2 fixture work must include OCR/text and structured color coverage cases when assigned.

For builder phases:

- Output package paths must be shown.
- Validator results must be included.
- RC2 builder work must report `/text/` and `/colors/` outputs once those phases are assigned.
- First-video-trial work must include text/color evidence, including queryable scene or shot color summaries.
