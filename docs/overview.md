# SVP Overview

SVP, or Semantic Video Package, is a proposed open standard for storing a strict, complete, machine-readable representation of video and time-based media.

The core idea is that every video should be able to carry a standardized observation layer:

- original media
- extracted audio
- transcript
- per-word timestamps
- speaker segments
- shot boundaries
- scene boundaries
- visual tracks
- spatial regions
- relationships
- depth maps
- masks
- embeddings
- search index
- provenance
- validation metadata

SVP is designed so video tools, AI agents, editors, search systems, and automation pipelines can reason over the media without repeatedly decoding and re-analyzing the raw video.

SVP is not an AI-generated metadata format. It is a media observation package.

AI may be used by a reference builder to produce some required observations, but SVP Core itself is model-agnostic. Labels and interpretations are non-core.
