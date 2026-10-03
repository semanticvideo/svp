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

The job always runs `<root>/current/bin/svp-builder worker serve --root
<root>`, where `<root>/current` is a symlink owned by the worker's user with
the relative target `runtimes/<runtime-id-hex>`. Workers update themselves:
when a coordinator pushes a runtime that is newer than the one the service
runs (by its release stamp, the UTC second `cmake --install` wrote into the
runtime's `libexec/svp/runtime/release.json`; ties go to the larger runtime
id), the service waits until no
session is live, verifies it, test-starts it (`worker verify-runtime`),
swaps `current`, and exits with status 75 (`EX_TEMPFAIL`) so launchd starts it
again on the new runtime. A runtime that fails its test-start is not adopted
and the reason is logged in `<root>/logs/agent.log`; the service never moves
to an older or unstamped runtime, and old runtimes are not deleted.
Coordinators on older runtimes keep working with an updated worker while
their protocol major version matches.

Fleets pair new Macs without SSH and without a coordinator key on them:

```
svp-builder workers fleet init                    # first coordinator: create the fleet secret
svp-builder workers fleet token                   # a worker token (valid 7 days; --valid-days <n>, at most 90)
svp-builder workers fleet token --coordinator     # the fleet secret, for another coordinator
svp-builder workers fleet join -                  # on another coordinator: paste that token
svp-builder workers fleet pair [--models ...]     # pair, supply, and calibrate every joinable worker
svp-builder worker install --join -               # on a new worker Mac: paste a worker token
```

`worker install --join` runs on the new Mac itself; its administrator
password is asked once, by `sudo`, to install the LaunchDaemon with the
runtime that svp-builder belongs to (`/Library/Application Support/SVP/Worker`,
owned by that user). The Mac then advertises itself to its fleet over
Bonjour. Every coordinator holding the fleet secret pairs it on
`workers fleet pair` or before its next `build --distributed`. The pairing
runs over the SVP transport: P-256 ECDH and a transcript signed with the
fleet's key, so only a coordinator that holds the fleet secret can pair a
worker, and nobody who watches the network learns the resulting key. A
worker token cannot pair Macs or pose as a coordinator. After a worker's
first pairing it stays pairable by every coordinator of the fleet, even
once its token has expired. Fleet secrets live in
`~/Library/Application Support/SVP/Fleet/fleet.json` (0600). Tokens read
from stdin with `-` stay out of shell history. `workers unpair` cannot reach
a worker paired through its fleet; `--forget` removes this Mac's record.

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

`--distributed` runs the OCR stage's frame batches and the visual tracking
stage's windows (one task per tracking window) on this Mac and on every
ready paired worker, and, inside their own stages, the per-item work of
evidence crops, text embeddings, shot-keyframe embeddings, and depth; every
other stage, and the tracking stage's fold of its windows (identity across
windows, every ID, the masks and entity files), stays on this Mac, and
stages run in the same order as in a local build. Before any task runs, each
worker is reached by pairing id, refused if its macOS or the build's thread
plan does not match, and sent what it lacks: the runtime, the model bundles
(the tracking detector, depth, and visual embedding bundles too when
tracking runs), and the source media (BLAKE3-verified). Each Mac runs as many
tasks of each kind at once as its calibration found worthwhile (tracking is
measured on a short synthetic window with the build's tracking options, once
per tracking quality), and tasks are sized from this Mac's measured seconds
per item. Workers only compute: IDs, files, and blocks are written on this
Mac in the local build's order, and any item or tracking window a worker
could not compute cleanly is computed again on this Mac, so a package
records only failures a local build would. A build without `--distributed`
runs tracking as one stage, exactly as before. Workers decode with the
coordinator's ffmpeg build only (a different `ffmpeg -version` is refused).
A worker that cannot be reached is reported and skipped (`--require-workers`
fails the build instead when fewer are ready); a worker lost mid-build has
its batches run again elsewhere; `--resume --distributed` continues from the
journal. Output does not depend on which or how many Macs ran a build.
Without `--distributed` nothing above is loaded and no socket is opened.
Run the coordinator from Terminal or an SSH session in the foreground: a
process detached from them is subject to Local Network privacy and cannot
reach workers.
