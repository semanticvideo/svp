# OCR Performance Notes

This note records OCR performance experiments for the SVP builder. The goal is to make OCR meaningfully faster without changing OCR behavior, reducing quality, running ASR, or allowing memory use to drift outside a conservative runtime envelope.

## Current Read

The best proven CPU path is bounded recognition parallelism.

Recognition input canvases now stay at the model's native 320-pixel width for
ordinary text and expand in aligned steps only when the crop requires it,
bounded by the existing 3200-pixel long-text limit. This removed unnecessary
padding, reduced the measured CPU path from 42.37 seconds and 878 MB peak RSS
to 27.32 seconds and 702 MB peak RSS, and improved obvious text reads in the
measured sample.

Both measurements used `/Users/domesposito/Projects/samples/test-30.mp4`, a
34-frame OCR sampling pass over 3840x2160 source media, on an Apple M4 Max with
48 GB unified memory. The builder used the default `background` OCR profile:
two recognition workers with the parallel threshold left at 16 boxes. The
baseline was the parent implementation, which padded every recognition crop to
48x3200; the comparison used this branch's aligned dynamic-width canvases. Each
revision was measured with the same command, changing only the output and
diagnostic paths between runs:

```sh
SVP_BUILDER_DIAG_LOG=/tmp/svp-ocr-width.jsonl \
SVP_BUILDER_MEMORY_LIMIT_MB=2048 \
/usr/bin/time -l ./build/tools/svp-builder/svp-builder build \
  /Users/domesposito/Projects/samples/test-30.mp4 \
  --out /tmp/svp-ocr-width.svp \
  --staging-dir /tmp/svp-ocr-width-stage \
  --model-cache /Users/domesposito/Projects/svp-model-cache \
  --ffmpeg /opt/homebrew/bin/ffmpeg \
  --ffprobe /opt/homebrew/bin/ffprobe \
  --stop-after foundation-ocr \
  --progress none
```

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

## Planned Tests Before Targeted Sweep

The next tests should preserve OCR output identity while reducing CPU pressure.

Recommended order:

1. Compare workers 4 and workers 6 on the same gator partial/full diagnostic shape with CPU diagnostics enabled.
2. Split detector and recognizer ONNX thread settings so a recognizer-only thread cap can be tested without slowing detector inference.
3. Test workers 6 with a recognizer-only thread cap.
4. Decide whether the product should expose separate foreground and background profiles:
   - Foreground fast: workers 6, if CPU pressure is acceptable.
   - Background balanced: lower worker count or recognizer-thread-limited workers, depending on the next test result.

## Targeted CPU Spike Sweep

Purpose:

- Re-test only the gator region that showed the highest CPU pressure in the partial full-video run.
- Avoid waiting for broad OCR sampling to drift into the heavy region.
- Preserve quality and behavior.
- Keep the test bounded.

Source:

- `/Users/domesposito/Desktop/gator/new-gator.mp4`

Derivation:

- The previous partial full-video run showed the worst local CPU pressure around `frame_000157` through `frame_000161`.
- That local region averaged about 1,202% process CPU.
- The gator file duration is 9,521,002,667 us.
- The OCR sampler safe end is 9,520,902,667 us.
- With `target_max_samples=600`, the effective gap is 15,868,171 us.
- The targeted diagnostic window used sample indices 146 through 161:
  - `2316752966,2332621137,2348489308,2364357479,2380225650,2396093821,2411961992,2427830163,2443698334,2459566505,2475434676,2491302847,2507171018,2523039189,2538907360,2554775531`

Shared command shape:

- OCR-only.
- `--stop-after foundation-ocr`.
- `SVP_OCR_DIAG_TIMESTAMPS_US` set to the 16 timestamps above.
- `SVP_OCR_RECOGNITION_PARALLEL_MIN_BOXES=16`.
- `SVP_BUILDER_MEMORY_LIMIT_MB=2048`.
- No ASR and no full build.

Results:

| Setting | Run root | Wall to last OCR frame | Avg CPU | Peak RSS | Frame sum | Recognition sum | Output identity |
| --- | --- | ---: | ---: | ---: | ---: | ---: | --- |
| Workers 6 | `gator-heavycpu-parw6-default-20260703-124855` | 35,096 ms | 1,082.91% | 1,364,525,056 bytes | 33,086.10 ms | 30,477.71 ms | Control |
| Workers 6, recognizer intra-op 1 | `gator-heavycpu-parw6-recintra1-20260703-125008` | 78,710 ms | 389.81% | 1,534,214,144 bytes | 76,708.87 ms | 74,129.90 ms | Incomplete output run; rejected on speed |
| Workers 6, recognizer intra-op 2 | `gator-heavycpu-parw6-recintra2-20260703-125304` | 63,754 ms | 543.00% | 1,393,131,520 bytes | 61,739.25 ms | 59,160.38 ms | Differs |
| Workers 4 | `gator-heavycpu-parw4-default-20260703-125522` | 38,128 ms | 963.17% | 1,178,845,184 bytes | 36,102.22 ms | 33,510.38 ms | Byte-identical |
| Workers 3 | `gator-heavycpu-parw3-default-20260703-125729` | 39,278 ms | 895.11% | 1,151,025,152 bytes | 37,290.19 ms | 34,713.16 ms | Byte-identical |

Output identity comparison:

- Workers 4 and workers 3 matched the workers-6 control byte-for-byte for:
  - `text_observations.jsonl`
  - `text_regions.jsonl`
  - `numeric_values.jsonl`
  - `evidence_crops.jsonl`
  - `text_absence.json`
  - `processors.jsonl`
- Workers 6 with recognizer intra-op 2 changed serialized confidence values in OCR outputs. Even when text looked the same, this is still an output change, so this path is not acceptable as a quality-preserving optimization.
- Workers 6 with recognizer intra-op 1 was more than 2x slower than the uncapped workers-6 control and was rejected on speed before treating it as a candidate path.

Interpretation:

- Recognizer-only ONNX thread caps reduce CPU, but the current tested caps are not good product candidates:
  - Intra-op 1 cuts CPU hardest but gives back too much speed.
  - Intra-op 2 still gives back too much speed and changes serialized OCR outputs.
- Reducing worker count is safer than capping recognizer ONNX threads in this build:
  - Workers 4 keeps output identity, reduces peak RSS by about 185 MB versus workers 6, and cuts average CPU by about 120 percentage points on the spike window.
  - Workers 3 keeps output identity, reduces peak RSS by about 214 MB versus workers 6, and cuts average CPU by about 188 percentage points on the spike window.
  - Workers 3 is only about 3.0% slower than workers 4 on this window while using less CPU and memory.

Current recommendation:

- Keep workers 6 as the foreground fast candidate.
- Treat workers 2 as the default background-safe candidate.
- Treat workers 3 as a faster conservative one-off candidate.
- Treat workers 4 as a possible future balanced candidate if workers 3 proves too slow on broader samples.
- Do not use recognizer ONNX thread caps for production profiles unless later tests prove an output-identical configuration.
- Add `timestamp_us` to OCR frame diagnostics so future heavy-window tests can be derived directly from diagnostics instead of reconstructing sample timestamps from the sampling contract.

## CLI Profile Validation

Change:

- Added `svp-builder build --ocr-performance serial|background|conservative|fast`.
- `serial` maps to 1 OCR recognition worker.
- `background` maps to 2 OCR recognition workers.
- `conservative` maps to 3 OCR recognition workers.
- `fast` maps to 6 OCR recognition workers.
- The selected profile is recorded in builder output and OCR diagnostics.
- Environment overrides remain available for diagnostics.

Validation shape:

- OCR-only.
- `--stop-after foundation-ocr`.
- No ASR and no full build.
- Same heavy timestamp windows used in the previous tests.
- `SVP_BUILDER_MEMORY_LIMIT_MB=2048`.

Results:

| File | Profile | Workers | Wall to last OCR frame | Avg CPU | Peak RSS | Output identity |
| --- | --- | ---: | ---: | ---: | ---: | --- |
| `ultimate-2.mp4` | conservative | 3 | 37,374 ms | 930.06% | 1,195,687,936 bytes | Control |
| `ultimate-2.mp4` | fast | 6 | 33,022 ms | 1,174.23% | 1,464,188,928 bytes | Byte-identical |
| `new-gator.mp4` | conservative | 3 | 39,325 ms | 895.68% | 1,152,909,312 bytes | Control |
| `new-gator.mp4` | fast | 6 | 35,574 ms | 1,087.11% | 1,362,001,920 bytes | Byte-identical |

Run roots:

- `/Users/domesposito/Projects/svp/build/diagnostics/profile-ultimate-conservative-20260703-130654`
- `/Users/domesposito/Projects/svp/build/diagnostics/profile-ultimate-fast-20260703-130929`
- `/Users/domesposito/Projects/svp/build/diagnostics/profile-gator-conservative-20260703-131157`
- `/Users/domesposito/Projects/svp/build/diagnostics/profile-gator-fast-20260703-131313`

Initial conclusion:

- The profile approach is good.
- Conservative keeps the footprint lower and remains much faster than the original single-worker baseline.
- Fast is meaningfully quicker when the user wants foreground speed.
- Both profiles preserved OCR output identity on the validation windows.
- Both profiles stayed below the 2 GB memory limit.

## Four-Profile Model

Profiles:

| Profile | Recognition workers | Intended use |
| --- | ---: | --- |
| `serial` | 1 | Lowest concurrency and closest behavior to the original single-worker path. |
| `background` | 2 | Default lower-footprint mode where OCR may share the machine with other pipeline stages. |
| `conservative` | 3 | Faster bounded one-off profile: meaningful speedup while keeping memory and CPU reasonable. |
| `fast` | 6 | Foreground speed profile for users who want the faster OCR pass and can spend more CPU. |

Validation plan:

- Validate `serial` and `background` on the known heavy windows for:
  - `/Users/domesposito/Projects/samples/ultimate-2.mp4`
  - `/Users/domesposito/Desktop/gator/new-gator.mp4`
- OCR-only.
- `--stop-after foundation-ocr`.
- No ASR and no full build.
- `SVP_BUILDER_MEMORY_LIMIT_MB=2048`.
- Compare staged OCR outputs against the matching `conservative` control for the same video/window.

Validation results:

| File | Profile | Workers | OCR stage wall | Frame sum | Recognition sum | Avg OCR CPU | Peak RSS | Peak footprint | Output identity |
| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | --- |
| `ultimate-2.mp4` | serial | 1 | 161,936 ms | 46,712.11 ms | 45,894.82 ms | 352.85% | 815.83 MB | 799.95 MB | Byte-identical |
| `ultimate-2.mp4` | background | 2 | 153,410 ms | 38,348.24 ms | 37,538.48 ms | 367.06% | 949.80 MB | 934.02 MB | Byte-identical |
| `new-gator.mp4` | serial | 1 | 83,824 ms | 46,404.40 ms | 43,853.72 ms | 555.92% | 810.78 MB | 794.92 MB | Byte-identical |
| `new-gator.mp4` | background | 2 | 77,289 ms | 39,877.84 ms | 37,297.11 ms | 601.58% | 931.03 MB | 915.27 MB | Byte-identical |

Run roots:

- `/Users/domesposito/Projects/svp/build/diagnostics/profile-ultimate-serial-20260703-145351`
- `/Users/domesposito/Projects/svp/build/diagnostics/profile-ultimate-background-20260703-145634`
- `/Users/domesposito/Projects/svp/build/diagnostics/profile-gator-serial-20260703-145908`
- `/Users/domesposito/Projects/svp/build/diagnostics/profile-gator-background-20260703-150032`

Validation comparison:

- Compared each new run against the matching `conservative` control for:
  - `text_observations.jsonl`
  - `text_regions.jsonl`
  - `numeric_values.jsonl`
  - `evidence_crops.jsonl`
  - `text_absence.json`
  - `processors.jsonl`
- All compared files were byte-identical.
- All four runs stayed below the 2 GB memory diagnostics limit.

Four-profile conclusion:

- `serial` is the lowest-footprint option and remains available for maximal background friendliness.
- `background` is the default low-impact practical option: it preserved behavior, stayed below 1 GB peak on both heavy windows, and improved speed versus `serial`.
- `conservative` remains the faster bounded one-off profile.
- `fast` remains the foreground speed profile.
