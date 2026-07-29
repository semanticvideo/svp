# Reference-model installer bootstrap licenses

`svp-models-tool install` uses a temporary conversion environment only while
reproducing the approved PP-OCR and RF-DETR files. The environment is deleted
before a successful install returns. It is never used by `svp build` and is not
copied into the model cache.

The pinned Python archive is the Apple Silicon `install_only` artifact from
`astral-sh/python-build-standalone` release `20250612`, containing CPython
3.11.13 and its machine-readable `PYTHON.json` component/license inventory.
The archive is MIT-licensed tooling around the included PSF-licensed CPython
distribution and bundled third-party components.

The locked wheels come from the Python Package Index and retain their package
metadata and license files. Their declared licenses are:

| Packages | License |
| --- | --- |
| `paddle2onnx`, `paddlepaddle`, `onnx`, `polygraphy`, `onnx-graphsurgeon`, `flatbuffers`, `ml-dtypes` | Apache-2.0 |
| `onnxruntime`, `onnxoptimizer`, `opt-einsum`, `coloredlogs`, `humanfriendly`, `h11`, `anyio` | MIT |
| `numpy`, `astor`, `httpx`, `httpcore`, `idna`, `networkx`, `sympy`, `mpmath` | BSD-3-Clause |
| `decorator` | BSD-2-Clause |
| `protobuf` | BSD-3-Clause |
| `packaging` | Apache-2.0 OR BSD-2-Clause |
| `typing-extensions` | PSF-2.0 |
| `certifi` | MPL-2.0 |
| `Pillow` | HPND |

The installer invokes pip with dependency resolution disabled and requires the
exact SHA-256 hashes in the two committed lock files. A changed, substituted,
or incompatible wheel is rejected rather than used.
