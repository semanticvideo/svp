# Embedded SVPI System Support on macOS

Status: implementation design retained for the `svp-support` application

Transport specification:
[`Embedded_SVPI_Transport_ISO_BMFF_v1.md`](../svpi/Embedded_SVPI_Transport_ISO_BMFF_v1.md)

## 1. Product behavior

The macOS support application should open the transcript and inspector for an
ordinary ISO BMFF media file that contains Embedded SVPI Transport. The file
remains an `.mp4`, `.mov`, `.m4v`, or `.m4a` and must continue to use the
system's normal media player, Quick Look provider, and media reader.

Finder should expose these application services for supported movie files:

- `Show Embedded SVPI Transcript`
- `Embedded SVPI Inspector`

The service entry points are intentionally stable even when the selected media
does not contain semantics. A normal movie produces a clear, non-destructive
message:

```text
This media does not contain embedded SVPI.
```

This behavior is deliberate. macOS `NSServices` can filter file URLs by
Uniform Type Identifier, but it cannot inspect ISO BMFF boxes before Finder
constructs the Services menu. Finder Sync can construct content-aware menus,
but only inside configured monitored directories. System-wide availability is
more important than hiding an inapplicable action.

Do not introduce a Finder Sync extension, monitored-directory preference,
custom embedded-MP4 filename extension, or custom document type for this
feature.

## 2. Ownership boundary

The macOS path should remain native Swift and reuse the existing `SVPReader`
semantic implementation:

```text
physical media file
    -> ISO BMFF top-level scanner
    -> validated Embedded SVPI envelope and payload range
    -> bounded random-access source
    -> existing ZipReader
    -> existing SVPReader APIs
    -> existing transcript and inspector windows
```

The transport layer owns only:

- structural ISO BMFF detection;
- top-level box walking;
- registered UUID discovery;
- envelope parsing and version checks;
- payload bounds and multiplicity;
- BLAKE3 verification of the exact SVPI bytes; and
- creation of a bounded, seekable package source.

It must not parse transcript, OCR, colors, entities, relationships, indexes,
provenance, or other SVPI semantics. Those remain owned by `SVPReader`.

## 3. Swift package-source refactor

The existing Swift `ZipReader` treats the physical file as a ZIP beginning at
offset zero. Embedded SVPI requires a logical byte source whose offset zero is
the validated SVPI payload offset.

Introduce a small random-access contract with checked offsets:

```swift
protocol SVPPackageByteSource: Sendable {
    var length: UInt64 { get }
    func read(offset: UInt64, length: Int) throws -> Data
}
```

Provide two implementations:

- `FilePackageByteSource`: a complete standalone `.svp` or `.svpi` file.
- `BoundedPackageByteSource`: a validated subrange inside another seekable
  source.

`BoundedPackageByteSource` must:

- translate logical offsets to `baseOffset + logicalOffset` with checked
  unsigned arithmetic;
- reject every read outside its declared length;
- use `pread` or an equivalently independent positional read;
- avoid changing shared file-handle cursor state; and
- never allocate from an untrusted declared payload length.

Refactor `ZipReader` to read from this source instead of opening a URL and
assuming physical offset zero. ZIP central-directory and local-header offsets
then remain relative to the logical SVPI source, so the semantic reader does
not need embedded-media conditionals.

The public package opener should resolve a URL once:

```text
standalone SVP/SVPI -> full-file source
supported ISO BMFF with one valid embedding -> bounded source
supported ISO BMFF without embedding -> noEmbeddedSvpi
unsupported or malformed input -> specific transport error
```

## 4. ISO BMFF discovery

Input detection is structural, not extension-based. The Swift scanner must
mirror Version 1 of the transport specification:

1. Read a valid leading `ftyp` box.
2. Classify the major and compatible brands.
3. Walk top-level box headers using their declared sizes.
4. Support both 32-bit sizes and 64-bit `largesize` headers.
5. Treat size zero as extending to EOF.
6. For `uuid`, read the 16-byte user type.
7. Match UUID `e2b6a23c-22ca-5636-b165-991208c837f1`.
8. Read and validate the 64-byte Version 1 envelope.
9. Require exactly one active embedding.
10. Return the exact bounded payload offset and length.

Discovery must seek over `mdat`. It must not scan media bytes, search for a raw
UUID pattern, decode media, or load the complete movie into memory.

The Version 1 envelope uses big-endian fields:

| Offset | Width | Meaning |
| ---: | ---: | --- |
| 0 | 8 | `SVPIMP4\0` magic |
| 8 | 2 | profile version `1` |
| 10 | 2 | envelope size `64` |
| 12 | 4 | zero flags |
| 16 | 8 | exact SVPI payload length |
| 24 | 32 | BLAKE3-256 of the SVPI bytes |
| 56 | 8 | zero reserved bytes |

Use one named transport-profile definition. Do not scatter UUID, magic,
version, or envelope offsets through application code.

## 5. Integrity and validation

Opening an embedded package for transcript or inspector use requires:

- supported structural container classification;
- one embedding box;
- supported profile version and zero flags;
- valid envelope and payload bounds;
- exact payload-length agreement;
- matching BLAKE3-256; and
- successful existing SVPI package validation.

CryptoKit does not provide BLAKE3. The Swift package should expose a pinned,
auditable BLAKE3 implementation through a focused SwiftPM target. The normal
runtime must not invoke a CLI, Python, or a network service.

Hash only the bounded SVPI payload, never the complete containing movie.
Transport discovery may remain a header-only probe; full integrity verification
occurs before the transcript or inspector consumes package entries.

## 6. Application integration

Keep the existing `.svp` and `.svpi` services unchanged. Add separate movie
service methods that accept system movie file URLs and pass them through a
single semantic-media resolver.

The resolver should return a typed state rather than a Boolean:

```swift
enum SemanticMediaStatus {
    case embedded(package: SVPPackageDescriptor)
    case noEmbeddedSvpi
    case invalidEmbedding(EmbeddedSVPIError)
    case unsupportedContainer
}
```

Initial behavior:

- `embedded`: present the existing transcript or inspector window.
- `noEmbeddedSvpi`: present the no-data message.
- `invalidEmbedding`: present a transport-integrity diagnostic.
- `unsupportedContainer`: present an unsupported-container diagnostic.

The transcript loader and every inspector section must accept the resolved
package source or descriptor. They must not independently rescan and rehash the
movie for every lazy-loaded section. Cache the descriptor for the lifetime of
the window, keyed by stable file identity, size, and modification date.

The inspector window title should identify the representation as
`Embedded SVPI`, while the overview may show:

- container family and brands;
- embedding profile version;
- UUID box and payload offsets/sizes; and
- payload integrity status.

Existing package sections and views should remain unchanged because the
bounded source exposes the same canonical SVPI.

## 7. macOS components that should not change

Do not register a new Uniform Type Identifier for embedded media. The same
file must remain a normal system movie.

Do not register the SVP MediaExtension as an MP4/MOV reader. Native readers
must continue handling media playback.

Do not replace the system Quick Look provider for MP4/MOV. Native Quick Look
already previews the containing media.

Do not materialize the entire SVPI to a temporary file for routine transcript
or inspector access.

## 8. Future status abstraction

The stable Finder actions can later surface a broader processing state without
changing service registration:

```text
embedded and valid
sidecar available
queued for processing
currently processing
processing incomplete
processing failed
semantic data stale
not yet analyzed
```

That future resolver may consult local sidecars and processing state. It must
not be invented as part of the initial embedded-reader implementation. The
initial scope is embedded, absent, malformed, and unsupported.

## 9. Verification requirements

Use small generated fixtures for normal and hostile cases, including:

- normal MP4 without embedding;
- valid embedded MP4;
- unrelated UUID box;
- truncated top-level header;
- invalid and overflowing box sizes;
- unsupported envelope version;
- truncated envelope;
- payload-length mismatch;
- BLAKE3 mismatch; and
- duplicate embedding boxes.

Use these paired real files for application integration:

```text
/Users/domesposito/Projects/svp/build/ASR Test/C9802.MP4
/Users/domesposito/Projects/svp/build/ASR Test/C9802-semantic.MP4
```

Prove that:

- the services accept both files system-wide;
- the ordinary file produces the no-data state;
- the semantic file opens the transcript window;
- the semantic file opens every existing inspector section;
- inspector values match the extracted standalone SVPI;
- detection does not read `mdat`;
- no media decode occurs during discovery; and
- repeated lazy section loads reuse the resolved package descriptor.

The `svp-support` implementation must be developed in its own branch and
worktree. This document does not add that implementation to the C++ SVP
repository.
