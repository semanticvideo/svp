# Repository licensing

SVP uses explicit path-based licensing. An archive or generated copy retains
the license of its source material; packaging does not relicense a file.

| Material | License |
| --- | --- |
| Reference implementation, build files, scripts, implementation documentation, and repository-authored synthetic test fixtures | Apache License 2.0 ([`LICENSE`](LICENSE)) |
| Everything under `spec/`, including specifications, companion specifications, schemas, registries, review material, and generated DOCX/PDF forms | CC0 1.0 Universal ([`LICENSES/CC0-1.0.txt`](LICENSES/CC0-1.0.txt)) |
| `docs/svpi/SVPI_v0.1_Draft_Specification.md` and `docs/svpi/Embedded_SVPI_Transport_ISO_BMFF_v1.md` | CC0 1.0 Universal |
| Verbatim or generated copies of `spec/` material inside `releases/` or `distribution/` | CC0 1.0 Universal |
| Other implementation and planning documents inside a release archive | Apache License 2.0 unless the archive states otherwise |
| Third-party dependencies and bundled upstream material | Their respective upstream licenses and notices |
| Model bundles | The `LICENSE` and `NOTICE` included in each bundle; the repository license does not cover model weights |

The recorded WAV files under `fixtures/audio/sherpa-diarization/` are excluded
from the root Apache-2.0 grant. Their redistribution license and attribution
are tracked separately in issue #123. Until that directory carries its own
completed license notice, the repository grants no redistribution permission
for those recordings.

Generated `.svp` fixture archives inherit the license of their inputs. The
repository-authored synthetic OCR/color fixtures are Apache-2.0 test material;
an archive made from third-party or separately licensed media retains the
terms applicable to that media.

Binary distributions must include the root `LICENSE` and `NOTICE`, the CC0
text when specification material is included, and the third-party notices
described in [`THIRD_PARTY_NOTICES.md`](THIRD_PARTY_NOTICES.md).
