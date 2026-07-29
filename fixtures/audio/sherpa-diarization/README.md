# Sherpa diarization fixtures

These WAV fixtures are deterministic speaker-count probes for the real
sherpa-onnx diarization path.

These audio files are test fixtures and are included with permission.

They are 16 kHz mono PCM files derived with FFmpeg from local preview clips:

- `anne-preview.m4a`
- `dean-preview.m4a`
- `sharon-preview.m4a`
- `theo-preview.m4a`
- `speakers.MOV` default AAC audio stream

The source clips are short, so the fixtures repeat them across longer speaker
turns. Multi-speaker fixtures use 10-second speech blocks separated by
6-second pauses; this proves bounded Sherpa diarization can invalidate a local
speaker assignment after a real gap and still reconcile returning speakers.
`similar-timbre-two-speaker.wav` is a direct 16 kHz mono extraction from the
default audio stream of `speakers.MOV`; the video is not required by the test.

Run the real-model fixture assertions with:

```bash
SVP_SHERPA_DIARIZATION_MODEL_DIR=/path/to/model_sherpa_onnx_diarization \
  ./build/packages/svp-audio/svp-audio-tests
```
