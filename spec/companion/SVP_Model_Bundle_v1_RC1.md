# SVP Model Bundle v1 - RC1

SVP Model Bundles (`.svpmodel`) are ZIP64 packages used by the reference builder to distribute pinned, verified model artifacts without requiring a Python runtime.

## Canonical identity

All normative model references use canonical SVP-native `model_` IDs. Registry slugs are metadata only.

Example canonical model ID:

```text
model_depth_anything_v2_small
```

Example source metadata:

```json
{
  "source_registry": "github",
  "source_slug": "DepthAnything/Depth-Anything-V2"
}
```

## Bundle ID

`model_bundle_id` is derived as:

```text
<model_id>@<model_version>+blake3_<first_12_hex_of_bundle_blake3>
```

The full `bundle_blake3` field remains authoritative for verification.

## Canonical bundle digest

`bundle_blake3` is the lowercase 64-hex BLAKE3 digest of the following byte
stream. Multi-byte integers are unsigned 64-bit integers in network byte order.
Text is UTF-8 and lengths count bytes, not characters.

1. Begin with the bytes `SVP_MODEL_BUNDLE_BLAKE3_V1` followed by one zero byte.
2. Build the logical file inventory from the fixed root paths
   `model.svpmodel.json`, `model-lock.json`, `LICENSE`, and `NOTICE`, followed by
   every `files[].path` declared by the manifest. A logical path is a relative
   UTF-8 path using `/` separators. Empty paths, absolute paths, empty, `.` or
   `..` components, backslashes, NUL bytes, invalid UTF-8, and duplicate entries
   within `files[]` are forbidden. A `files[]` declaration matching a fixed root
   path describes that existing logical record and does not add a second record.
   A `LICENSE` declaration MUST use role `license`; a `NOTICE` declaration MUST
   use role `notice`.
3. Resolve every logical path below the extracted bundle root without following
   symbolic links. Each logical path MUST resolve to exactly one regular file,
   and each physical regular file MUST satisfy exactly one logical path. A
   missing file, undeclared extra file, symbolic link, socket, device, hard-link
   alias, Unicode-normalization alias, or other collision invalidates the
   bundle. Directories are permitted but are not records.
4. Preserve the manifest's logical path UTF-8 bytes exactly. Filesystem-returned
   spelling is never hashed. No Unicode normalization is performed: NFC and NFD
   spellings are distinct logical identities even on a filesystem that resolves
   them to the same physical spelling. Sort records by unsigned bytewise
   lexicographic order of those logical UTF-8 paths.
5. Append the file count as one unsigned 64-bit integer.
6. For each sorted file append, in order: byte `0x01`, the path byte length, the
   path bytes, the content byte length, and the content bytes.

Regular files include empty files, nested manifest-declared files, `LICENSE`,
`NOTICE`, and the two root control files. An undeclared file invalidates the
bundle rather than acquiring an implementation-dependent name in the digest.
Directories themselves are not records. File permissions,
timestamps, ownership, platform metadata, archive compression, and archive
entry order are not hashed.

Two root control files require a deterministic projection before their content
bytes are framed:

- `model.svpmodel.json` is parsed as JSON. Its top-level `bundle_blake3` is
  replaced with `blake3:` plus 64 zeroes. Its top-level `model_bundle_id` is
  replaced with `<model_id>@<model_version>+blake3_` plus 12 zeroes.
- `model-lock.json` is parsed as JSON. In every `models` entry, `bundle_blake3`
  and `model_bundle_id` receive the same replacements derived from that entry's
  `model_id` and `model_version`.

Object member names MUST be unique at every level. Duplicate names invalidate a
control file before projection, even if a JSON library would otherwise keep the
first or last occurrence. JSON numbers are interpreted from their exact decimal
tokens, independent of a parser's native number type:

- Mathematical zero, including negative zero and exponent spellings, becomes
  unsigned integer zero.
- A mathematically integral value, including forms such as `1.0` and `1e0`,
  becomes a CBOR integer and MUST be in the range negative 2^63 through
  2^64 minus 1.
- A non-integral value is rounded once to IEEE 754 binary64 using
  round-to-nearest, ties-to-even. Overflow, a non-finite result, or underflow of
  a nonzero decimal value to binary64 zero invalidates the control file.

The projected JSON value is encoded using RFC 8949 major types with this exact
deterministic profile: JSON null is `0xf6`; false and true are `0xf4` and `0xf5`;
non-negative integers use major type 0; negative integer `n` uses major type 1
with argument `-1-n`; integer arguments and definite lengths use the shortest
permitted additional-information width; floating-point values are always
`0xfb` followed by IEEE 754 binary64 in network byte order; strings use major
type 3 and their UTF-8 bytes; arrays use major type 4 and preserve order; and
objects use major type 5 with members ordered by unsigned bytewise
lexicographic comparison of their UTF-8 keys. Tags, indefinite lengths, and
non-finite numbers are forbidden. This profile is normative and does not depend
on any particular JSON or CBOR library.

This removes only the two recursive digest representations; all other manifest
and lock metadata remains covered. A file with either control filename below
the bundle root is an ordinary file and is not projected.

The length framing makes path/content concatenations unambiguous. The fixed
domain prefix prevents these bytes from being confused with another BLAKE3 use.
Verification MUST compare this computed value with `bundle_blake3` after normal
manifest validation and per-file digest validation.

Independent control-number projection vectors are:

```text
1, 1.0, 1e0  -> 01
-0, -0.0     -> 00
0.5, 5e-1    -> fb3fe0000000000000
```

## Required manifest fields

A conforming `model.svpmodel.json` contains `model_bundle_id`, canonical `model_id`, `model_version`, `bundle_blake3`, runtime, format, license, supported execution providers, files, input contract, output contract, preprocessor contract, and postprocessor contract.

`model.svpmodel.json` is authoritative for a single bundle. `model-lock.json` is authoritative for the selected set of bundles. If they disagree on identity or hashes, the builder rejects the model set.
