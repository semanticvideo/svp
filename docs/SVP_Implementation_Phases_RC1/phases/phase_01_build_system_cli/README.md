# Phase 01 - Build System, Dependency Manifest, and CLI Skeleton

## Phase purpose

Create the native C++20 project skeleton that every later phase will build on. This is real infrastructure: CMake, vcpkg, shared libraries, CLI commands, and first executable targets.

## Prerequisites

- Phase 00 committed.
- Repo root is clean.

## Primary outputs

```text
CMakeLists.txt
vcpkg.json
packages/svp-core/
packages/svp-package/
packages/svp-validation/
tools/svp-validator/
tools/svp-inspector/
tools/svp-builder/
```

## Work items

1. Add root `CMakeLists.txt` with C++20 enabled.
2. Add `vcpkg.json` with initial dependencies: CLI11, nlohmann-json, sqlite3, libzip, blake3, zstd.
3. Create `packages/svp-core` with version constants, timestamp types, ID helpers, path helpers, and hash string types.
4. Create `packages/svp-validation` with validation report structs and JSON serialization.
5. Create `packages/svp-package` with placeholder package probing API.
6. Create `tools/svp-validator` executable with `--help`, `--version`, and `validate` command stub.
7. Create empty build targets for inspector and builder if useful, but do not implement their behavior yet.

## Required commands

```bash
cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE="$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake"
cmake --build build
./build/tools/svp-validator/svp-validator --help
./build/tools/svp-validator/svp-validator --version
```

## Definition of done

- CMake configures.
- Project builds.
- `svp-validator --help` works.
- `svp-validator --version` prints the active SVP spec/tool version.
- No media processing code is added.

## Handoff report format

At the end of this phase, report:

```text
Phase: Phase 01 - Build System, Dependency Manifest, and CLI Skeleton
Status: complete / blocked / partial
Files created:
Files modified:
Commands run:
Tests passed:
Tests failed:
Known issues:
Next recommended phase:
```
