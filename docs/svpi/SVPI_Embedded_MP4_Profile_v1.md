# SVPI Embedded in ISO Base Media File Format / MP4, Version 1

Status: production transport profile

Profile ID: `svpi-embedded-mp4-v1`

Byte order: big-endian (network order)

## 1. Purpose and format relationship

This profile stores one complete canonical SVPI inside an ordinary ISO Base
Media File Format / MP4 file. It is a transport for SVPI, not an MP4-native
semantic format:

```text
MP4 + embedded canonical SVPI
```

SVPI remains the independently versioned source of truth for semantic
observations, indexes, provenance, and media binding. The profile does not
duplicate transcript, OCR, numeric text, colors, entities, relationships,
embeddings, index data, or media identity in MP4 boxes. The raw SVPI payload is
identical to the sidecar form and is not compressed by this transport.

SVP remains the self-contained representation containing primary media and
semantics. Embedded MP4 remains playable without SVP-aware software.

## 2. Registered UUID

The top-level ISO BMFF box type is `uuid`. Its 16-byte user type is:

```text
e2b6a23c-22ca-5636-b165-991208c837f1
```

The UUID is deterministic UUIDv5 using the standard URL namespace UUID
`6ba7b811-9dad-11d1-80b4-00c04fd430c8` and this exact name:

```text
https://semanticvideo.org/spec/svpi-embedded-mp4/v1
```

The value and derivation are permanently registered in
`spec/registries/mp4-embedding-profiles.json`. Implementations must use the
registered constant rather than independent anonymous literals.

## 3. UUID box layout

The box uses the standard ISO BMFF 32-bit size form when possible:

| Relative offset | Width | Field |
| ---: | ---: | --- |
| 0 | 4 | `size` |
| 4 | 4 | ASCII `uuid` |
| 8 | 16 | registered SVPI user type |
| 24 | 64 | Version 1 envelope |
| 88 | N | exact canonical SVPI bytes |

The compact total box size is `88 + N`. It is used when that value is at most
`0xffffffff`; therefore the maximum compact SVPI payload is 4,294,967,207
bytes.

Larger payloads use the standard extended-size form:

| Relative offset | Width | Field |
| ---: | ---: | --- |
| 0 | 4 | value `1` |
| 4 | 4 | ASCII `uuid` |
| 8 | 8 | `largesize` = `96 + N` |
| 16 | 16 | registered SVPI user type |
| 32 | 64 | Version 1 envelope |
| 96 | N | exact canonical SVPI bytes |

All sizes and offsets use checked unsigned 64-bit arithmetic. A box smaller
than its applicable header or extending outside the physical file is invalid.

## 4. Version 1 envelope

The envelope is exactly 64 bytes:

| Envelope offset | Width | Type | Meaning |
| ---: | ---: | --- | --- |
| 0 | 8 | bytes | magic `53 56 50 49 4d 50 34 00` (`SVPIMP4\0`) |
| 8 | 2 | uint16 | profile version, value `1` |
| 10 | 2 | uint16 | envelope size, value `64` |
| 12 | 4 | uint32 | flags; Version 1 requires zero |
| 16 | 8 | uint64 | exact SVPI payload length `N` |
| 24 | 32 | bytes | BLAKE3-256 of the exact `N` SVPI bytes |
| 56 | 8 | bytes | reserved; Version 1 requires all zero |

The raw SVPI begins immediately after the declared envelope. Version 1
requires the declared payload length to equal all bytes remaining in the UUID
box. Readers must verify bounds before following the range. The BLAKE3 value
protects only the embedded SVPI bytes; this profile does not hash the resulting
MP4 and does not create a circular media identity.

## 5. Discovery

Readers walk only top-level box headers from offset zero to EOF:

1. Read the 8-byte box header.
2. Read `largesize` only when `size == 1`.
3. Reject impossible sizes, overflow, truncation, or boxes outside the file.
4. For `uuid`, read the 16-byte user type.
5. For the registered user type, read and validate only the 64-byte envelope.
6. Seek over every other box using its declared size.

Detection must not scan raw bytes, search inside `mdat`, decode media, or load
the file or SVPI into memory. Its I/O is proportional to the number of
top-level boxes. After a valid range is found, package readers open the SVPI
through a bounded seekable source so existing ZIP64, manifest, index, query,
and validation code is reused.

## 6. Placement and preservation

For a supported non-fragmented MP4, the canonical writer appends the UUID box
at EOF. This leaves all original bytes and all existing absolute media offsets
unchanged.

When the terminal top-level box is `mfra`, the writer inserts the UUID box
immediately before `mfra`. `mfra` offsets refer to earlier media fragments, so
the original tail bytes and referenced fragments remain unchanged while
`mfra` remains terminal. A non-terminal `mfra` or a top-level `mfro` is rejected
as an unsupported tail layout.

A top-level box with size zero extends to EOF. Version 1 writers reject such
files because inserting after it would make the new bytes part of that box.
Malformed or ambiguous top-level layouts are rejected; writers never guess.

Embedding is a byte-level insertion, not an FFmpeg remux. Every original byte
is copied unchanged. Stripping removes the one registered UUID box. For files
produced from a clean MP4 by this implementation, stripping reconstructs the
original MP4 byte-for-byte.

## 7. Multiplicity and replacement

Version 1 permits exactly one active SVPI UUID box. Readers report duplicates
as invalid and never choose one. Writers refuse to add another embedding
unless explicit replacement is requested. Explicit replacement removes all
registered top-level embedding boxes and writes one canonical replacement.
This profile does not define history or generations.

## 8. Validation and media binding

Validation has two separate layers:

1. MP4 transport: top-level structure, UUID multiplicity, profile/envelope,
   bounds, exact length, and payload BLAKE3.
2. Bounded SVPI: the existing SVPI package validator and existing Core and
   authenticity statuses.

When `media_binding.json` contains a present full-file BLAKE3, embedded
validation hashes the logical clean MP4 byte stream (all bytes except the SVPI
UUID box) and compares it with that binding. Embedding does not replace SVPI's
media identity or provenance contract.

Registered transport validation codes include:

- `ERR_MP4_BOX_STRUCTURE_INVALID`
- `ERR_MP4_UUID_BOX_TRUNCATED`
- `ERR_MP4_SVPI_PROFILE_UNSUPPORTED`
- `ERR_MP4_SVPI_ENVELOPE_INVALID`
- `ERR_MP4_SVPI_PAYLOAD_BOUNDS`
- `ERR_MP4_SVPI_PAYLOAD_LENGTH_MISMATCH`
- `ERR_MP4_SVPI_PAYLOAD_HASH_MISMATCH`
- `ERR_MP4_SVPI_DUPLICATE`
- `ERR_MP4_SVPI_NOT_FOUND`
- `ERR_MP4_EMBEDDED_SVPI_INVALID`
- `ERR_MP4_UNSAFE_TAIL_LAYOUT`
- `ERR_MP4_ZERO_SIZED_TOP_LEVEL_BOX`
- `ERR_MP4_SVPI_MEDIA_BINDING_MISMATCH`

## 9. CLI

Build any supported output representation from media:

```bash
svp-builder build source.mp4 --out package.svp --output-format svp
svp-builder build source.mp4 --out package.svpi --output-format svpi
svp-builder build source.mp4 --out semantic.mp4 --output-format embedded-mp4
```

Embed an existing canonical SVPI:

```bash
svp-builder interlace embed-mp4 source.mp4 package.svpi --out semantic.mp4
```

Use `--replace-existing` only to replace an existing embedding and
`--overwrite` only to atomically replace an existing output path.

Inspect, validate, query, extract, and strip:

```bash
svp-inspector inspect semantic.mp4 --json
svp-validator validate semantic.mp4 --json
svp-inspector query semantic.mp4 --mode transcript --json
svp-builder interlace extract-embedded semantic.mp4 --out package.svpi
svp-builder interlace strip-embedded semantic.mp4 --out clean.mp4
```

Extraction validates the envelope and payload hash, then writes the exact SVPI
bytes atomically. If the bytes are recoverable but the contained SVPI is
invalid, extraction preserves the recovered artifact and reports package
validation failure. Stripping also validates the envelope and hash before
removal.

Existing SVP conversions compose through the canonical artifacts rather than
using a reduced embedded representation:

```text
SVP -> interlace extract -> clean MP4 + canonical SVPI -> embed-mp4
embedded MP4 -> extract-embedded + strip-embedded -> recombine -> SVP
```

These paths reuse the existing SVP extractor/recombiner, media binding checks,
SVPI writer, and validators. No package-building logic is duplicated in the
MP4 transport layer.

## 10. Preservation expectations and limitations

An ordinary byte-for-byte copy preserves the embedding. Remuxing,
transcoding, metadata cleaning, social-media processing, or editing/export
software may discard unknown top-level boxes. Applications must not claim that
third-party exports preserve embedded SVPI unless that exact workflow was
tested.

Version 1 deliberately rejects ambiguous size-zero layouts, non-terminal
`mfra`, and top-level `mfro`. It does not define an MP4-native semantic schema,
front-of-file locator, payload compression, append-only history, or complete
MP4 hash. It never requires media decoding for discovery and never requires a
Python runtime or network service.
