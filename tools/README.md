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
| `--staging-dir <dir>` | Use a specific staging directory |
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
