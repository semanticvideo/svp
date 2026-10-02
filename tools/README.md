# SVP Tools

This folder contains the reference CLIs for validating, inspecting, building,
querying, and interlacing SVP-family artifacts.

Historically the tools were built in this order:

1. svp-validator
2. svp-inspector
3. svp-builder

The validator remains the spine of the project. Builder output is not complete
until it is inspectable and validator-clean for the target artifact type.

## svp-validator

Validates package structure, schemas, binary blocks, hashes, SQLite logical row streams, validation code usage, and equivalence behavior.

### Package equivalence (RC2 Sections 5.16 and 17.5)

```
svp-validator validate --equivalent <a.svp> <b.svp> [--json] [--ignore-build-metadata]
svp-validator validate --equivalent <a.svpi> <b.svpi> [--json] [--ignore-build-metadata]
```

Reports exactly one of `byte_identical`, `structurally_equivalent`,
`numerically_equivalent`, or `not_equivalent` under the Default Equivalence
Profile v1, with per-entry findings (rule, first differing record or field,
measured value and tolerance). Exit status is 0 for the three equivalent
classes and 1 for `not_equivalent`.

Wall-clock and build-host path fields (for example `manifest.json`
`created_utc`) are compared exactly by default. `--ignore-build-metadata`
normalizes only the fields in the comparator's build-metadata registry and
lists every normalized field in the report. Embedded SVPI transport
containers are not accepted; compare their extracted `.svpi` sidecars.

## svp-inspector

Human-readable and agent-readable package inspection CLI.

`svp-inspector` is the single command for package summaries, JSON dumps, and
semantic queries. The old separate `svp-reader` executable plan was folded into
library reader APIs plus this CLI to avoid splitting package debugging across
two commands.

### Dump package metadata

```
svp-inspector dump <package.svp> --section manifest
svp-inspector dump <package.svp> --section index_manifest
```

### Query modes

```
svp-inspector query <package.svp> --mode <mode> [options]
```

| Mode | Description |
|------|-------------|
| `layers` | List all package layers with record counts |
| `transcript` | Transcript summary (language, word/speaker counts) |
| `words` | Search words by text (`--text`, `--limit`) |
| `speakers` | List speakers with word counts |
| `ocr` | List OCR text observations (`--text`, `--limit`) |
| `colors` | List color observations (`--bucket`, `--min-coverage`, `--limit`) |
| `validation` | Show validation report status |
| `relationships` | List relationships with class breakdown (`--class`, `--limit`, `--json`) |
| `traverse` | Traverse relationship graph from a starting object (`--from`, `--depth`, `--direction`, `--class`, `--type`, `--limit`, `--json`) |

### Relationship traversal

```
svp-inspector query <pkg.svp> --mode traverse --from <object_id> \
  [--depth N] [--direction outgoing|incoming|both] \
  [--class support|semantic|unknown|all] [--type <relationship_type>] \
  [--limit N] [--json]
```

Builds an in-memory graph from `relationships/relationships.jsonl` and traverses
outward from the starting object. Nodes are annotated with compact summaries
from the package's object catalog (words, OCR observations, frames, speakers,
text regions, entities, crops, color observations, masks, depth, embeddings).

Unresolved endpoint IDs (referenced by relationships but not found in any
catalog layer) are reported in `missing_object_ids`.

### Relationship listing

```
svp-inspector query <pkg.svp> --mode relationships [--class support|semantic|unknown|all] [--limit N] [--json]
```

## svp-builder

Processes source media into SVP packages and SVPI sidecars.

### Build `.svp`

```
svp-builder build <source-media> --out <package.svp> [options]
```

Common options:

| Option | Description |
|--------|-------------|
| `--staging-dir <dir>` | Use a specific staging directory (preserved after build; default staging is removed on success) |
| `--model-cache <dir>` | Use local SVP model cache |
| `--ffmpeg <path>` | FFmpeg executable |
| `--ffprobe <path>` | ffprobe executable |
| `--sherpa-lib <path>` | sherpa-onnx C API library for diarization |
| `--stop-after <stage>` | Diagnostic partial-stage stop after `media-ingest`, `audio`, `vision-plan`, `foundation-color`, `foundation-ocr`, or `package` |
| `--ocr-performance <profile>` | OCR profile: `serial`, `background`, `conservative`, or `fast` (default: `background`) |
| `--allow-fallback-diarization` | Allow explicit fallback when sherpa-onnx is unavailable |
| `--force-single-speaker` | Intentionally skip diarization and declare one speaker |

### SVPI interlace commands

SVPI is the sidecar/interlace format for semantic observations bound to source
media. It keeps the original media outside the sidecar while preserving
semantic records, indexes, provenance, and media identity binding.

```
svp-builder interlace create <source-media> --out <sidecar.svpi> [options]
```

Creates a semantic `.svpi` sidecar. By default this runs the semantic build
pipeline and writes the generated observations into the sidecar. Use
`--core-only-diagnostic` only when intentionally creating a diagnostic
core-only sidecar.

```
svp-builder interlace inspect <sidecar.svpi> [--json]
```

Shows SVPI manifest, binding, identity, index/provenance presence, section
states, and recombination readiness.

```
svp-builder interlace validate <sidecar.svpi> [--media <source-media>] [--json]
```

Validates SVPI structure. When `--media` is supplied, also verifies that the
candidate media satisfies the sidecar's media binding.

```
svp-builder interlace extract <package.svp> --out-dir <dir>
```

Extracts both the embedded source media and a matching `.svpi` sidecar from a
full `.svp` package.

```
svp-builder interlace recombine <source-media> <sidecar.svpi> --out <package.svp>
```

Verifies binding and recombines source media plus SVPI observations into a full
`.svp` package.

```
svp-builder interlace create-batch <media-dir> [--recursive] [--sidecar-visibility visible|hidden|managed-dir]
svp-builder interlace scan <media-dir> [--recursive] [--json]
svp-builder interlace validate-batch <media-dir> [--recursive] [--json]
svp-builder interlace complete-identity <sidecar.svpi> --media <source-media>
svp-builder interlace complete-identity-batch <media-dir> [--recursive] [--json]
```

Batch create skips already-valid bound sidecars by default. Use
`--replace-mismatched` only when intentionally replacing sidecars that fail
binding verification.

### SVPI media hygiene

SVPI sidecars must not contain primary media or replayable source-derived media.
The writer filters forbidden entries and the validator rejects bad sidecars.

Forbidden examples:

```
media/original/
media/audio/original_stream_000.flac
media/audio/analysis_mono_16k.wav
evidence/audio_clip.wav
media/derived/proxy_video.mp4
```

Allowed examples:

```
media/audio/waveform.jsonl
media/audio/audio_absence.json
transcript/words.jsonl
transcript/speaker_segments.jsonl
text/evidence_crops/*.jpg
media_binding.json
```

### Worker Macs (macOS)

Pair another Apple Silicon Mac running the same macOS version as a worker for
distributed builds. Pairing uses the system `ssh` once (keys or a password
typed into ssh itself; SVP never handles passwords):

```
svp-builder workers pair <user>@<host> [--system-service] [--models all|none|<ids>] [--dry-run]
svp-builder workers list [--json]
svp-builder workers status [<pairing-id>|<user>@<host>] [--json]
svp-builder workers sync <pairing-id>|<user>@<host> [--models all|none|<ids>]
svp-builder workers unpair <pairing-id>|<user>@<host> [--forget]
```

`pair` checks arm64, the macOS product version, and free disk; copies this
Mac's runtime (svp-builder plus its runtime bundle when one is installed,
otherwise svp-builder alone) and has the worker verify every file's BLAKE3;
writes a 256-bit pairing secret on both Macs (0600); and installs a launchd
job that only accepts connections. The default is a LaunchAgent for the
worker's user (`~/Library/Application Support/SVP/Worker`); with
`--system-service` it is a LaunchDaemon that runs as that user without anyone
logged in (`/Library/Application Support/SVP/Worker`, installed with `sudo`).
Workers are found by pairing id over Bonjour, never by address. `unpair`
removes the job, runtimes, models, cache, and the secret on both Macs.
`--dry-run` writes the plist and scripts locally, lints them, and changes
nothing on the worker.

`pair` and `sync` also measure capacity on this Mac and on the worker, for
OCR and for each kind of vision work a distributed build sends (evidence
crops, text and keyframe embeddings, depth): a short synthetic slice runs at
1, 2, ... concurrent tasks until another slot adds less than 10% throughput
(or memory and CPUs admit no more). The results are kept beside the pairings
(`.../SVP/Calibration/`) and measured again whenever the runtime, macOS,
hardware, thread counts, decoder build, or model bundles change; a
distributed build measures any that are missing before it starts.

Distributed builds:

```
svp-builder build <source> --out <out.svp> --distributed [--require-workers <n>]
```

`--distributed` runs the OCR stage's frame batches on this Mac and on every
ready paired worker, and, inside their own stages, the per-item work of
evidence crops, text embeddings, shot-keyframe embeddings, and depth; every
other stage stays on this Mac, and stages run in the same order as in a local
build. Before any task runs, each worker is reached by pairing id, refused if
its macOS or the build's thread plan does not match, and sent what it lacks:
the runtime, the model bundles, and the source media (BLAKE3-verified). Each
Mac runs as many tasks of each kind at once as its calibration found
worthwhile, and tasks are sized from this Mac's measured seconds per item.
Workers only compute: IDs, files, and blocks are written on this Mac in the
local build's order, and any item a worker could not compute is computed
again on this Mac, so a package records only failures a local build would. Workers decode with the
coordinator's ffmpeg build only (a different `ffmpeg -version` is refused).
A worker that cannot be reached is reported and skipped (`--require-workers`
fails the build instead when fewer are ready); a worker lost mid-build has
its batches run again elsewhere; `--resume --distributed` continues from the
journal. Output does not depend on which or how many Macs ran a build.
Without `--distributed` nothing above is loaded and no socket is opened.
Run the coordinator from Terminal or an SSH session in the foreground: a
process detached from them is subject to Local Network privacy and cannot
reach workers.
