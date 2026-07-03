# OCR Performance Notes

This note records OCR performance experiments for the SVP builder. The goal is to make OCR meaningfully faster without changing OCR behavior, reducing quality, running ASR, or allowing memory use to drift outside a conservative runtime envelope.

## Current Read

The best proven CPU path is bounded recognition parallelism.

- Workers 6 is the conservative fast candidate:
  - About 35.5% faster than the default heavy-frame control.
  - Peak RSS about 1.50 GB on the `ultimate-2.mp4` heavy-frame sweep.
  - Staged OCR outputs were byte-identical to the default control.
- Workers 8 is the fastest tested candidate:
  - About 37.9% faster than the default heavy-frame control.
  - Peak RSS about 1.90 GB, which is inside the 2 GB diagnostic limit but leaves little headroom.
- CoreML is not acceptable in the current shape:
  - It was proven buildable in a temp ONNX Runtime build.
  - It exceeded the OCR memory budget immediately, reaching about 3.40 GB RSS before completing the first OCR frame.

The open issue is CPU pressure. Workers 6 preserves the speedup and memory behavior, but it can drive high CPU on text-heavy frames because each recognition worker calls ONNX Runtime, and ONNX Runtime can also use internal CPU threading.

## Machine Context

The current test machine is an Apple M4 Max laptop with 48 GB unified memory.

Apple publishes M4 Max MacBook Pro configurations with up to:

- 16-core CPU.
- 40-core GPU.
- 48 GB unified memory in the relevant MacBook Pro configuration.
- 546 GB/s memory bandwidth on the 16-core CPU / 40-core GPU configuration.

Apple also describes the top M4 Max CPU as up to 12 performance cores plus 4 efficiency cores.

Sources:

- Apple MacBook Pro 16-inch M4 Max technical specifications: <https://support.apple.com/en-us/121554>
- Apple M4 Pro and M4 Max announcement: <https://www.apple.com/newsroom/2024/10/apple-introduces-m4-pro-and-m4-max/>

## Constraints

- Do not run ASR for OCR performance tests.
- Do not run the full build unless explicitly requested.
- Use `--stop-after foundation-ocr` for OCR-only builder tests.
- Preserve OCR quality and behavior:
  - No downscaling as a speed fix.
  - No semantic recognition changes.
  - No output-changing shortcuts.
- Keep runtime memory conservative:
  - The user-approved experiment range is about 1-2 GB.
  - Around 3 GB is not acceptable.
- Keep raw run artifacts in `build/diagnostics/`, but keep human-readable notes under `docs/` so they can be committed.

## Key Implementation Knobs

The tested branch currently includes instrumentation and runtime knobs for OCR performance investigation:

- `SVP_OCR_RECOGNITION_PARALLEL_WORKERS`
- `SVP_OCR_RECOGNITION_PARALLEL_MIN_BOXES`
- `SVP_OCR_ONNX_INTRA_OP_THREADS`
- `SVP_OCR_ONNX_INTER_OP_THREADS`
- `SVP_OCR_ONNX_GRAPH_OPT_LEVEL`
- `SVP_OCR_ONNX_EXECUTION_MODE`
- `SVP_OCR_EXECUTION_PROVIDER`
- `SVP_OCR_DIAG_TIMESTAMPS_US`
- `SVP_BUILDER_MEMORY_LIMIT_MB`

CPU diagnostics were added to memory diagnostics so each diagnostics event can record process CPU time:

- `user_cpu_ms`
- `system_cpu_ms`
- `total_cpu_ms`

## Heavy-Frame Control

Source:

- `/Users/domesposito/Projects/samples/ultimate-2.mp4`

Diagnostic timestamps:

- `20000000,21000000,99000000,100000000,101000000`

These timestamps target known OCR-heavy regions with many text boxes.

Default complete control:

- Run root: `/Users/domesposito/Projects/svp/build/diagnostics/ultimate-2-ocr-heavy-default-complete-20260703-113320`
- Frame sum: 45,938.04 ms.
- Recognition sum: 45,127.96 ms.
- Peak RSS: 850,640,896 bytes.
- Total boxes / recognition attempts: 253 / 253.

## Recognition Worker Sweep

All rows below used OCR-only builder runs, the same five heavy timestamps, `--stop-after foundation-ocr`, no ASR, and a 2 GB memory diagnostics limit.

| Setting | Run root | Frame sum | Recognition sum | Peak RSS | Output identity |
| --- | --- | ---: | ---: | ---: | --- |
| Default | `ultimate-2-ocr-heavy-default-complete-20260703-113320` | 45,938.04 ms | 45,127.96 ms | 850,640,896 bytes | Control |
| Workers 2 | `ultimate-2-ocr-heavy-parw2-20260703-113016` | 39,057.70 ms | 38,240.21 ms | 1,004,109,824 bytes | Byte-identical |
| Workers 3 | `ultimate-2-ocr-heavy-parw3-20260703-113654` | 33,567.80 ms | 32,762.48 ms | 1,195,376,640 bytes | Byte-identical |
| Workers 4 | `ultimate-2-ocr-heavy-parw4-20260703-113946` | 30,872.63 ms | 30,030.02 ms | 1,267,957,760 bytes | Byte-identical |
| Workers 5 | `ultimate-2-ocr-heavy-parw5-20260703-114237` | 30,510.63 ms | 29,689.81 ms | 1,437,073,408 bytes | Byte-identical |
| Workers 6 | `ultimate-2-ocr-heavy-parw6-20260703-114524` | 29,609.09 ms | 28,794.03 ms | 1,494,941,696 bytes | Byte-identical |
| Workers 8 | `ultimate-2-ocr-heavy-parw8-20260703-114810` | 28,533.92 ms | 27,674.96 ms | 1,904,377,856 bytes | Byte-identical |

Output identity means these staged OCR outputs matched the default control byte-for-byte:

- `text_observations.jsonl`
- `text_regions.jsonl`
- `numeric_values.jsonl`
- `evidence_crops.jsonl`
- `text_absence.json`
- `processors.jsonl`

`foundation-ocr.json` differed only in absolute staging paths.

## CPU Diagnostics From Long Webinar Sample

Source:

- `/Users/domesposito/Desktop/gator/new-gator.mp4`
- 1920x1080.
- 25 fps.
- Duration: 9,521.002667 seconds, about 2:38:41.
- File size: 2,689,163,952 bytes.

Partial full-video OCR run:

- Run root: `/Users/domesposito/Projects/svp/build/diagnostics/gator-full-ocr-parw6-20260703-121549`
- `SVP_OCR_RECOGNITION_PARALLEL_WORKERS=6`
- `SVP_OCR_RECOGNITION_PARALLEL_MIN_BOXES=16`
- `SVP_BUILDER_MEMORY_LIMIT_MB=2048`
- OCR-only, no ASR, `--stop-after foundation-ocr`.
- Stopped manually after enough sample data was collected.

Observed result:

- Completed OCR frames: 156.
- Last completed frame: `frame_000161`.
- OCR elapsed time to last completed frame: 202,688 ms.
- Frame processing sum: 183,137.87 ms.
- Recognition processing sum: 158,872.24 ms.
- Total boxes recognized: 1,132.
- Max boxes in one frame: 36.
- Peak RSS: 1,412,759,552 bytes.
- Peak footprint: 1,396,541,120 bytes.
- Memory diagnostics limit exceeded: false.

CPU result:

- Total process CPU delta: 1,702,963 ms.
- Average CPU from first diagnostic event to last event: 838.25%.
- Average CPU from OCR start to last completed OCR frame: 839.58%.
- Slow text-heavy region from `frame_000157` through `frame_000161`:
  - Wall-clock delta: 15,415 ms.
  - CPU delta: 185,323 ms.
  - Average CPU over that local region: about 1,202%.

Interpretation:

- Memory stayed inside the conservative runtime envelope.
- CPU was high enough to matter for product behavior.
- On a 16-core M4 Max, 839% process CPU is about 8.4 fully busy cores on average.
- The local 1,202% region is about 12 fully busy cores, which lines up with the top M4 Max performance-core count.
- Workers 6 is a good foreground speed candidate, but it may be too aggressive as the default for future background operation.

## Gator Targeted Window

Target:

- Source: `/Users/domesposito/Desktop/gator/new-gator.mp4`
- User timestamp: `01:54:53:20`.
- At 25 fps, frame 20 is 0.8 seconds, so the target is 6,893.8 seconds.
- Diagnostic window: 21 one-second samples from target minus 10 seconds through target plus 10 seconds.

Run:

- Run root: `/Users/domesposito/Projects/svp/build/diagnostics/gator-ocr-window-parw6-20260703-120447`
- Workers 6.
- OCR-only, no ASR, `--stop-after foundation-ocr`.

Result:

- Completed successfully.
- OCR frame count: 21.
- Frame sum: 26,252.05 ms.
- Recognition sum: 22,842.20 ms.
- Total boxes / attempts: 171 / 171.
- Max boxes in one sampled frame: 20.
- Full-run peak RSS: 1,386,332,160 bytes.
- Full-run peak footprint: 1,374,045,864 bytes.
- Memory diagnostics limit exceeded: false.

Conclusion:

- Late-file seeking and a local text-heavy webinar window are stable with workers 6.
- This short window does not prove there is no memory buildup across a full 2.5 hour OCR run.
- The partial full-video run above gives better evidence that memory remains bounded over a longer sample.

## Rejected Paths

### Recognition Batching

Recognition batching was tested before the parallel-worker path.

| Setting | Result |
| --- | --- |
| Batch size 2 | Stayed below 2 GB but was much slower than default. |
| Batch size 4 | Stayed below 2 GB but remained slower than default. |
| Batch size 6 | Near 2 GB and still slower than default. |

Batching was rejected because it did not improve speed and increased memory.

### CoreML

CoreML was tested through a temp ONNX Runtime build under:

- `/Users/domesposito/Projects/svp/build/diagnostics/coreml-temp`

The temp build proved:

- `SVP_COREML_AVAILABLE=1`
- CoreML provider libraries linked.
- CoreML framework linked.

OCR test:

- Run root: `/Users/domesposito/Projects/svp/build/diagnostics/ultimate-2-ocr-heavy-coreml-20260703-112910`
- `SVP_OCR_EXECUTION_PROVIDER=coreml`
- Same five heavy OCR timestamps.
- 2 GB memory limit.

Result:

- Failed during first detector inference before completing frame 1.
- Exit status: 86 from memory diagnostics.
- Peak RSS at failure: 3,398,172,672 bytes.
- Peak footprint at failure: 3,312,143,744 bytes.

Conclusion:

- CoreML is buildable, but this path is rejected for now because it violates the memory budget immediately.

### ONNX Runtime CPU Knobs

These options did not produce a meaningful speedup in the tested shape:

| Setting | Result |
| --- | --- |
| `SVP_OCR_ONNX_INTRA_OP_THREADS=8` | Essentially tied with default. |
| `SVP_OCR_ONNX_GRAPH_OPT_LEVEL=all` | Tied with default; ONNX Runtime already defaults graph optimization to all. |
| `SVP_OCR_ONNX_EXECUTION_MODE=parallel`, `SVP_OCR_ONNX_INTER_OP_THREADS=2` | About 1% faster, not meaningful. |
| Workers 4 plus `SVP_OCR_ONNX_INTRA_OP_THREADS=1` | Slower because the cap also slowed detector inference. |

## Next Tests

The next tests should preserve OCR output identity while reducing CPU pressure.

Recommended order:

1. Compare workers 4 and workers 6 on the same gator partial/full diagnostic shape with CPU diagnostics enabled.
2. Split detector and recognizer ONNX thread settings so a recognizer-only thread cap can be tested without slowing detector inference.
3. Test workers 6 with a recognizer-only thread cap.
4. Decide whether the product should expose separate foreground and background profiles:
   - Foreground fast: workers 6, if CPU pressure is acceptable.
   - Background balanced: lower worker count or recognizer-thread-limited workers, depending on the next test result.
