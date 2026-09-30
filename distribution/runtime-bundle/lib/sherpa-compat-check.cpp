// Compatibility check between a built sherpa-onnx C API library and the SVP
// code that loads it with dlopen/dlsym.
//
// SVP does not include sherpa-onnx headers. It mirrors the C structs in
// packages/svp-audio/src/sherpa_diarization/private.hpp and resolves function
// pointers by name in packages/svp-audio/src/sherpa_diarization/api.cpp. A
// sherpa-onnx release that adds a struct field or drops a symbol silently
// breaks that contract, so the runtime bundle build compiles this file
// against both declarations and loads the built library.
//
// Compile time: every mirrored struct has the same size, alignment, and field
// offsets as the upstream declaration.
// Run time:     argv[1] is the library path; argv[2..] are the symbol names
//               SVP resolves. Exit status is non-zero on any failure.

#include "sherpa-onnx/c-api/c-api.h"

#include "private.hpp"

#include <cstddef>
#include <cstdio>
#include <dlfcn.h>

namespace svp_mirror = svp::audio::sherpa_diarization_internal;

#define SVP_SAME_LAYOUT(T)                                              \
  static_assert(sizeof(::T) == sizeof(svp_mirror::T), #T " size");      \
  static_assert(alignof(::T) == alignof(svp_mirror::T), #T " alignment")

#define SVP_SAME_FIELD(T, F)                                            \
  static_assert(offsetof(::T, F) == offsetof(svp_mirror::T, F), #T "." #F)

SVP_SAME_LAYOUT(SherpaOnnxOfflineSpeakerDiarizationSegment);
SVP_SAME_FIELD(SherpaOnnxOfflineSpeakerDiarizationSegment, start);
SVP_SAME_FIELD(SherpaOnnxOfflineSpeakerDiarizationSegment, end);
SVP_SAME_FIELD(SherpaOnnxOfflineSpeakerDiarizationSegment, speaker);

SVP_SAME_LAYOUT(SherpaOnnxOfflineSpeakerSegmentationPyannoteModelConfig);
SVP_SAME_FIELD(SherpaOnnxOfflineSpeakerSegmentationPyannoteModelConfig, model);

SVP_SAME_LAYOUT(SherpaOnnxOfflineSpeakerSegmentationModelConfig);
SVP_SAME_FIELD(SherpaOnnxOfflineSpeakerSegmentationModelConfig, pyannote);
SVP_SAME_FIELD(SherpaOnnxOfflineSpeakerSegmentationModelConfig, num_threads);
SVP_SAME_FIELD(SherpaOnnxOfflineSpeakerSegmentationModelConfig, debug);
SVP_SAME_FIELD(SherpaOnnxOfflineSpeakerSegmentationModelConfig, provider);

SVP_SAME_LAYOUT(SherpaOnnxSpeakerEmbeddingExtractorConfig);
SVP_SAME_FIELD(SherpaOnnxSpeakerEmbeddingExtractorConfig, model);
SVP_SAME_FIELD(SherpaOnnxSpeakerEmbeddingExtractorConfig, num_threads);
SVP_SAME_FIELD(SherpaOnnxSpeakerEmbeddingExtractorConfig, debug);
SVP_SAME_FIELD(SherpaOnnxSpeakerEmbeddingExtractorConfig, provider);

SVP_SAME_LAYOUT(SherpaOnnxFastClusteringConfig);
SVP_SAME_FIELD(SherpaOnnxFastClusteringConfig, num_clusters);
SVP_SAME_FIELD(SherpaOnnxFastClusteringConfig, threshold);

SVP_SAME_LAYOUT(SherpaOnnxOfflineSpeakerDiarizationConfig);
SVP_SAME_FIELD(SherpaOnnxOfflineSpeakerDiarizationConfig, segmentation);
SVP_SAME_FIELD(SherpaOnnxOfflineSpeakerDiarizationConfig, embedding);
SVP_SAME_FIELD(SherpaOnnxOfflineSpeakerDiarizationConfig, clustering);
SVP_SAME_FIELD(SherpaOnnxOfflineSpeakerDiarizationConfig, min_duration_on);
SVP_SAME_FIELD(SherpaOnnxOfflineSpeakerDiarizationConfig, min_duration_off);

int main(int argc, char** argv) {
  if (argc < 3) {
    std::fprintf(stderr, "usage: %s <libsherpa-onnx-c-api.dylib> <symbol>...\n", argv[0]);
    return 2;
  }
  // Same flags as svp::audio::sherpa_diarization_internal::get_api().
  void* handle = dlopen(argv[1], RTLD_LAZY | RTLD_LOCAL);
  if (handle == nullptr) {
    std::fprintf(stderr, "dlopen failed: %s\n", dlerror());
    return 1;
  }
  int missing = 0;
  for (int i = 2; i < argc; ++i) {
    if (dlsym(handle, argv[i]) == nullptr) {
      std::fprintf(stderr, "missing symbol: %s\n", argv[i]);
      ++missing;
    }
  }
  std::printf("struct layout ok; %d of %d symbols resolved\n", argc - 2 - missing, argc - 2);
  return missing == 0 ? 0 : 1;
}
