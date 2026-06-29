# SVP Tools

The tools should be built in this order:

1. svp-validator
2. svp-reader
3. svp-inspector
4. svp-builder

Do not start with the builder.

## svp-validator

First priority.

Validates package structure, schemas, binary blocks, hashes, SQLite logical row streams, validation code usage, and equivalence behavior.

## svp-reader

Reads package contents and exposes structured access.

## svp-inspector

Human-readable package inspection CLI.

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

Processes source media into SVP packages. This comes last.
