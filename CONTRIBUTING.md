# SVP Contribution Guide

The guidelines below describe how to contribute to the SVP repository.

## Goals

SVP is an open-source project, and contributions are not limited to code. Useful
contributions include:

- Improving documentation.
- Reviewing pull requests.
- Expanding validator and test coverage.
- Fixing bugs or typos.
- Improving the specification and reference tools.

## Issues

- Check existing issues, including closed issues, before opening a new one.
- Bug reports should include reproduction steps, expected and actual behavior,
  platform details, and relevant command output.
- Include a minimal test case when practical.

## Build Prerequisites

SVP currently supports Apple Silicon macOS. Install the prerequisites and use
the clean-clone vcpkg workflow documented in the [README](README.md#build).

## Pull Requests

### Getting Started

- Please do not open a pull request if you do not plan to see it through.
- Use the existing code style.
- Keep one logical change per pull request.
- Avoid unrelated refactors.
- Add relevant validation or tests when behavior changes.
- Update documentation when the pull request changes documented behavior.
- Create the pull request from a new branch, never directly from `main`.
- AI-assisted coding is welcome, but contributors remain responsible for the
  resulting code and review quality.

### Submission

- Run the relevant build, test, and validation commands.
- Give the pull request a clear title and description.
- Enable “Allow edits from maintainers.”
- Reference any issue the pull request fixes, such as `Fixes #123`.

### Review

- Push follow-up commits when addressing review feedback. Commits may be
  squashed when the pull request is merged.
- Review your diff before each update.
- Be patient with reviews; SVP is currently maintained by one person.

Thank you for contributing to SVP.
