# SVP Runtime Bundle: FFmpeg and sherpa-onnx

These scripts build the third-party executables and libraries that `svp-builder` runs, so that an SVP worker Mac needs nothing except macOS. Today SVP calls Homebrew's `ffmpeg`/`ffprobe` and loads sherpa-onnx from pip or Homebrew locations. Homebrew's ffmpeg links 19 Homebrew dylibs and is a GPL build, and the sherpa-onnx version is not pinned. The bundle replaces both with pinned, hashed, relocatable builds.

This directory only builds the bundle. `svp-builder` does not use it by default yet. Pass the files explicitly with `--ffmpeg`, `--ffprobe`, and `--sherpa-lib`.

## What is built

| Output | Upstream | Version | License | Linkage |
|---|---|---|---|---|
| `bin/ffmpeg`, `bin/ffprobe` | FFmpeg | 9.0.2 | LGPL-2.1-or-later | static FFmpeg libraries; OS libraries only |
| (inside ffmpeg/ffprobe) | dav1d | 1.5.4 | BSD-2-Clause | static |
| `lib/libsherpa-onnx-c-api.dylib` | sherpa-onnx | 1.13.5 | Apache-2.0 | `@rpath/libonnxruntime.1.dylib`, libc++, libSystem |
| `lib/libonnxruntime.1.dylib` | ONNX Runtime (official Microsoft release) | 1.27.1 | MIT | Apple frameworks and OS libraries |

`versions.env` owns every version, URL, and SHA-256, plus the macOS deployment target (15.0, the CI runner's macOS; the repository declares no lower minimum). Each archive is checked against its SHA-256 before it is used. The FFmpeg and dav1d hashes are the same ones Homebrew pins for its own builds of those versions.

### Why these versions

- **FFmpeg 9.0.2 and dav1d 1.5.4.** These are the versions SVP runs through Homebrew today. Keeping the same decoder source keeps decoded frames and samples identical; see [Verification](#verification).
- **sherpa-onnx 1.13.5.** SVP does not include sherpa-onnx headers. It loads the library with `dlopen`, looks up symbols with `dlsym`, and keeps its own copy of the C struct layouts in `packages/svp-audio/src/sherpa_diarization/private.hpp`. 1.13.5 is the newest release whose layout matches that copy:
  - 1.13.6 adds `window_shift_ratio` to the pyannote config, which moves every later config field.
  - 1.13.8 adds `compute_confidence` to the clustering config and a `confidence` field to each result segment, which changes the size of each segment in the result array.

  Moving to a newer release requires updating SVP's struct copy first. `build-sherpa-onnx.sh` enforces this with `lib/sherpa-compat-check.cpp`, which fails the build on a mismatch (tested against the 1.13.8 header).
- **ONNX Runtime 1.27.1.** This is the version sherpa-onnx 1.13.5 pins for osx-arm64. The script uses Microsoft's official release package of that version rather than the sherpa maintainer's repackaged archive.

## Output layout

```text
<out-dir>/                       default: <repo>/dist/runtime (ignored by git)
  bin/ffmpeg
  bin/ffprobe
  lib/libsherpa-onnx-c-api.dylib
  lib/libonnxruntime.1.dylib
  licenses/                      upstream license texts
  share/svp-runtime/
    ffmpeg-buildconf.txt         `ffmpeg -buildconf` of the built binary
    components/*.json            per-component build records
  source/                        verified upstream archives (with --with-source)
  components.json                every file with BLAKE3, size, and component version
```

`components.json` uses the schema `svp.runtime.components/1`. Every file is listed with its `blake3:<hex>` digest and size:

- Executables and libraries appear under the component that produced them. That entry also records the component's version, license, source URL and SHA-256, build flags, and statically or dynamically linked libraries.
- License texts, build records, and source archives are listed under `support_files`.

## How to rebuild

Host requirements:

- An Apple Silicon Mac with the Command Line Tools (Apple clang).
- `curl`, `shasum`, `make`, and `jq`. All of these ship with macOS.
- For FFmpeg: `meson`, `ninja`, and `pkg-config`/`pkgconf` (`brew install meson ninja pkgconf`). They are used only to build dav1d.
- For sherpa-onnx: `cmake` 3.24 or newer.

Nothing from a package-manager prefix is linked into the outputs. Each build runs with an empty environment and a `PATH` of `/usr/bin:/bin:/usr/sbin:/sbin`, plus symlinks to the named build tools only.

```bash
distribution/runtime-bundle/build-ffmpeg.sh      [--work-dir DIR] [--out-dir DIR] [--jobs N] [--with-source]
distribution/runtime-bundle/build-sherpa-onnx.sh [--work-dir DIR] [--out-dir DIR] [--jobs N] [--with-source]
distribution/runtime-bundle/write-components-manifest.sh [--out-dir DIR] \
    --hash-tool build/macos-arm64-release/tools/svp-models-tool/svp-models-tool
```

- The default work directory, `distribution/runtime-bundle/work/`, is ignored by git.
- `write-components-manifest.sh` hashes with `svp-models-tool hash`, or with `b3sum` if `--hash-tool` is omitted.
- The sherpa-onnx compatibility check compiles SVP's `private.hpp`, which needs `nlohmann/json_fwd.hpp`. The check takes that header from sherpa-onnx's fetched dependencies, or from an existing SVP build tree.

Each build script fails unless:

- every shipped Mach-O depends only on `/usr/lib`, `/System/Library`, or another shipped file through `@rpath`;
- the only rpath is `@loader_path`, and `ffmpeg`/`ffprobe` have no rpath at all;
- the Mach-O `minos` equals the pinned deployment target (the prebuilt ONNX Runtime dylib keeps Microsoft's `minos` 14.0);
- `ffmpeg` has every decoder, encoder, filter, demuxer, muxer, and input device listed in `ffmpeg-svp-features.txt`;
- `ffmpeg -L` reports the LGPL;
- the sherpa-onnx library passes `lib/sherpa-compat-check.cpp`. That check compares struct sizes and field offsets at compile time. At run time it `dlopen`s the built library and resolves every symbol name that `api.cpp` looks up.

## FFmpeg configure flags

Every flag has a comment in `build-ffmpeg.sh`. In summary:

| Flag | Reason |
|---|---|
| `--prefix=/usr/local` plus `make install DESTDIR=…` | Keeps work-tree paths out of the binaries. The in-tree build also keeps `__FILE__` relative. |
| `--cc=clang` | Apple clang from the Command Line Tools, the same compiler family as Homebrew's build |
| `--optflags=-Os` | Homebrew's bottle is compiled at `-Os`, not FFmpeg's default `-O3`, and SVP's reference outputs come from that bottle. At `-O3` the native Opus decoder (float) differs from the bottle by up to 4.5e-8 in about 17% of samples, which changes extracted FLAC bytes. At `-Os` the output is byte-identical. Integer decoders are unaffected. |
| `--disable-gpl --disable-version3 --disable-nonfree` | LGPL v2.1+ only. No x264, x265, or GPL filters, and nothing non-redistributable. |
| `--disable-shared --enable-static` | FFmpeg libraries linked into the two programs: no dylibs, no rpaths, relocatable |
| `--disable-autodetect` | Prevents linking whatever configure finds on the build host (SDL2, xz, libxcb, OpenSSL, …) |
| `--enable-pthreads` | Frame and slice threading, as in Homebrew's build |
| `--enable-zlib`, `--enable-bzlib` | OS zlib and bzip2: PNG, compressed MOV headers, bzip2-compressed Matroska tracks |
| `--enable-iconv --extra-libs=-liconv` | OS iconv for subtitle and MPEG-TS text. configure's probe passes without `-liconv`, but the static link then fails. |
| `--enable-libdav1d --pkg-config-flags=--static` | AV1 through dav1d, the decoder Homebrew selects today, linked statically |
| `--disable-ffplay --disable-doc --disable-network` | SVP runs only `ffmpeg` and `ffprobe` on local files |
| `--extra-cflags/--extra-ldflags=-mmacosx-version-min=15.0` | Pins the minimum macOS version |

Left off on purpose:

- VideoToolbox and AudioToolbox. SVP never requests a hardware decoder, and FFmpeg picks its built-in decoders first.
- lzma. It is only used for TIFF, and macOS ships no liblzma headers.
- SecureTransport. Networking is disabled.
- AVFoundation, AppKit, CoreImage, and Metal. SVP does not use capture devices or GPU filters.

All other FFmpeg-internal components (decoders, encoders, parsers, demuxers, muxers, filters) are left at their defaults, the same internal set Homebrew builds. The result links only `libSystem`, `libz`, `libbz2`, `libiconv`, and the CoreFoundation, CoreVideo, and CoreMedia frameworks. FFmpeg 9's libavutil links those three frameworks on macOS whenever it is built.

## Licensing and redistribution obligations

- **FFmpeg (LGPL-2.1-or-later).** `ffmpeg` and `ffprobe` are FFmpeg's own programs, built from unmodified upstream source. Anyone who redistributes them must provide the complete corresponding source: the FFmpeg and dav1d archives plus the scripts and configure flags in this directory. `--with-source` copies the verified archives into `source/` for that purpose. The LGPL relinking requirement applies to an application statically linked with the library. SVP does not link FFmpeg. It runs the programs as separate processes, and the programs are built entirely from FFmpeg and dav1d sources, so a user can rebuild or replace them with these scripts. The LGPL text is shipped in `licenses/FFmpeg-COPYING.LGPLv2.1`, together with FFmpeg's `LICENSE.md`.
- **dav1d (BSD-2-Clause).** Redistributions must keep the copyright notice and license: `licenses/dav1d-COPYING`.
- **sherpa-onnx (Apache-2.0).** Ship `licenses/sherpa-onnx-LICENSE`. The build statically links sherpa-onnx's fetched dependencies into the dylib: kaldi-native-fbank, kaldi-decoder, kaldifst, openfst, simple-sentencepiece, kissfft, eigen (header-only), hclust-cpp, and nlohmann-json (header-only). They are listed in `share/svp-runtime/components/sherpa-onnx.json`. Their notices must be added to `THIRD_PARTY_NOTICES.md` before a runtime bundle is published.
- **ONNX Runtime (MIT).** Ship `licenses/onnxruntime-LICENSE` and `licenses/onnxruntime-ThirdPartyNotices.txt`.

## Verification

The `verify/` scripts check the bundled binaries against the reference Homebrew ffmpeg 9.0.2:

- `verify/make-codec-samples.sh --encoder-ffmpeg /opt/homebrew/bin/ffmpeg --out DIR` generates short clips with Homebrew's encoders: H.264, HEVC 8-bit and 10-bit, ProRes, VP9, and AV1 (SVT-AV1) video, with AAC, PCM, Opus, and MP3 audio, plus one clip with two audio streams for the `amix` path.
- `verify/compare-decode.sh --ref-ffmpeg … --ref-ffprobe … --test-ffmpeg … --test-ffprobe … --out DIR clip…` runs both builds with SVP's command shapes and byte-compares the results:
  - probe JSON;
  - whole-clip `framemd5`;
  - one-frame `-ss` seeks to rgb24 at the canonical raster and at the ≤1920-wide OCR raster;
  - `fps`+`scale` sampling;
  - crop+scale+MJPEG evidence crops, and decoding those crops back to rgb24;
  - FLAC extraction;
  - 16 kHz mono s16le extraction, with and without `asetpts`/`aresample` and through `amix`;
  - `ebur128` through `ffprobe -f lavfi amovie`;
  - FLAC-to-f32le spectrum input.

### Recorded result (2026-09-30, macOS 27.0.1 on Apple Silicon, Apple clang 21.0.0)

- **Decode identity.** 204 of 204 comparisons were byte-identical against Homebrew ffmpeg 9.0.2. The inputs were a 30 s cut of the Gator fixture (H.264/AAC 1080p) and nine synthetic clips:
  - H.264/AAC;
  - H.264 with two AAC streams;
  - HEVC 8-bit/AAC;
  - HEVC 10-bit/AAC at 1280x720 and at 3840x2160;
  - ProRes 422/PCM;
  - VP9/Opus;
  - AV1/MP3;
  - AV1/Opus.

  Before `--optflags=-Os` was added, only the Opus rows differed (whole-clip decode and FLAC extraction).
- **Full SVP build.** `svp-builder build` on the Gator cut, once with Homebrew ffmpeg and once with the bundled binaries, with the same bundled sherpa-onnx. `svp-validator validate --equivalent A B --ignore-build-metadata` reported `structurally_equivalent`. All five findings derive from `created_utc`: the manifest field, its copy in `index.sqlite`, and the three index digests computed over them.
- **Diarization smoke.** `svp-builder diarize --sherpa-lib <bundle>/lib/libsherpa-onnx-c-api.dylib` on the repository's two-, three-, and four-speaker fixtures found 2, 3, and 4 speakers.
- **Relocation.** After the bundle was copied to another directory, both programs still ran, and `dlopen` loaded `libonnxruntime.1.dylib` from the new location.
