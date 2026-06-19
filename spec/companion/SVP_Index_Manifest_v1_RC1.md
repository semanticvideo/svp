# SVP Index Manifest v1 - RC1

`index/index_manifest.json` is a required SVP Core file. It records the logical integrity fingerprint of `index/index.sqlite` and gives validators enough information to verify the SQLite index without comparing raw SQLite database bytes.

The required schema file is `/schemas/index-manifest.schema.json`.

`logical_rows_blake3` is computed from the canonical logical row stream defined in SVP Section 17.5. `sqlite_file_blake3` is the exact BLAKE3 of the physical SQLite database file and is used for physical integrity diagnostics, not cross-package equivalence.
