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

For builder phases:

- Output package paths must be shown.
- Validator results must be included.
