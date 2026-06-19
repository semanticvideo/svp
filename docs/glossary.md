# SVP Glossary

## SVP

Semantic Video Package. A strict package format for time-based media observations.

## SVP Core

The required portion of every conforming `.svp` package.

## Label

A non-core interpretation attached to an observation. Labels are not required for SVP Core validity.

## Observation

A machine-readable fact about the media timeline, such as a word timestamp, visual region, depth value, mask, track, or relationship.

## Track

A time-persistent observed thing in the visual or audio timeline.

## Canonical Analysis Raster

The aspect-preserving raster size used by processors for depth, masks, optical flow, and spatial analysis.

## SVPB

The SVP binary block header/container used for depth, mask, embedding, and future tensor block streams.

## Index Manifest

The required `index/index_manifest.json` file that records logical SQLite index integrity.

## Logical Row Stream

A canonical serialization of SQLite table contents used to validate index equivalence without requiring byte-identical SQLite database files.

## Model Bundle

A pinned, content-addressed native model artifact bundle used by the reference builder.

## Signature Sidecar

An optional detached `.svpsig` authenticity file for verifying exact finalized `.svp` package bytes.
