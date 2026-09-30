# Third-party attribution policy

The repository does not relicense its dependencies, external tools, model
weights, or upstream legal material. A source checkout is not, by itself, a
complete third-party notice bundle for a compiled binary release.

Before publishing an SVP binary, the release process must:

1. Build from the committed `vcpkg.json` manifest and `builtin-baseline` so the
   resolved dependency set is reproducible.
2. Inventory every direct and transitive vcpkg package in the selected triplet.
3. Collect each installed port's `share/<port>/copyright` file and retain all
   required license, copyright, attribution, and NOTICE text.
4. Inventory linked or bundled components not supplied by vcpkg, including
   platform libraries, FFmpeg when redistributed, whisper.cpp, ONNX Runtime,
   OpenCV, sherpa-onnx, and any separately packaged runtime.
5. Produce a release `THIRD_PARTY_NOTICES` file that identifies the exact
   component and version and includes the notice material required by its
   license.
6. Ship that generated notice file with the root `LICENSE` and `NOTICE`.
7. Fail the release if a redistributed component lacks an identified license
   or required attribution text.

For a normal manifest-mode build, vcpkg installs port attribution files below:

```text
<install-root>/<triplet>/share/<port>/copyright
```

The release inventory must be derived from the actual install tree used for
the binary, not only from the top-level dependency list, because transitive
dependencies may impose additional obligations.

Reference model weights are not hosted by SVP and are not covered by the root
Apache-2.0 license. Every installed model bundle carries its own `LICENSE` and
`NOTICE`, and those files must remain with the bundle. Installer bootstrap
components are documented in
[`distribution/reference-models/INSTALL_BOOTSTRAP_LICENSES.md`](distribution/reference-models/INSTALL_BOOTSTRAP_LICENSES.md).

## Runtime bundle components

`distribution/runtime-bundle/` builds SVP's own media and diarization runtime
so a Mac needs nothing installed besides macOS. Building and using it
internally has no extra obligation. Publishing a built bundle redistributes
the components below: the release notice must include each component's
license text, and FFmpeg's corresponding source must be offered
(`build-ffmpeg.sh --with-source` copies the verified source archives into the
bundle).

| Component | Version | License | Shipped as |
| --- | --- | --- | --- |
| FFmpeg | 9.0.2 | LGPL-2.1-or-later (built `--disable-gpl --disable-nonfree`) | `ffmpeg`, `ffprobe` (static) |
| dav1d | 1.5.4 | BSD-2-Clause | linked into `ffmpeg`, `ffprobe` |
| sherpa-onnx | 1.13.5 | Apache-2.0 | `libsherpa-onnx-c-api.dylib` |
| ONNX Runtime (Microsoft release) | 1.27.1 | MIT | `libonnxruntime.1.dylib` |

Compiled into the sherpa-onnx library, at the versions pinned by sherpa-onnx
1.13.5:

| Component | License |
| --- | --- |
| kaldi-native-fbank | Apache-2.0 |
| kaldi-decoder | Apache-2.0 |
| kaldifst | Apache-2.0 |
| OpenFst | Apache-2.0 |
| simple-sentencepiece | Apache-2.0 |
| KISS FFT | BSD-3-Clause |
| Eigen | MPL-2.0 (some files BSD or other MPL-2.0-compatible licenses) |
| hclust-cpp (fastcluster) | BSD-2-Clause |
| nlohmann/json | MIT |
