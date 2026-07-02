#include "cli_context.hpp"

#include "svp/audio/sherpa_diarization.hpp"
#include "svp/core/memory_diagnostics.hpp"

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>

#if defined(__APPLE__)
#include <unistd.h>
#endif

namespace {

std::uint64_t diarize_memory_limit_bytes() {
  const char* value = std::getenv("SVP_BUILDER_MEMORY_LIMIT_MB");
  if (value != nullptr && *value != '\0') {
    char* end = nullptr;
    const unsigned long long mb = std::strtoull(value, &end, 10);
    if (end != value && mb > 0) {
      return static_cast<std::uint64_t>(mb) * 1024ULL * 1024ULL;
    }
  }
  return 5ULL * 1024ULL * 1024ULL * 1024ULL;
}

}  // namespace

int run_diarize_command(const DiarizeCliOptions& options) {
  namespace fs = std::filesystem;

  const fs::path wav_path(options.wav_path);
  const fs::path model_dir(options.model_dir);

  if (!fs::exists(wav_path)) {
    std::cerr << "svp-builder diarize: WAV not found: " << wav_path << "\n";
    return 1;
  }
  if (!fs::exists(model_dir)) {
    std::cerr << "svp-builder diarize: model dir not found: " << model_dir << "\n";
    return 1;
  }

  if (!options.sherpa_lib_path.empty()) {
    svp::audio::set_sherpa_lib_path(options.sherpa_lib_path);
  }

  std::ostringstream diag_name;
  diag_name << "svp-diarize-memory";
#if defined(__APPLE__)
  diag_name << "-" << static_cast<long long>(getpid());
#endif
  diag_name << ".jsonl";

  const fs::path diag_dir = fs::current_path() / "build" / "diagnostics";
  const fs::path diag_path = diag_dir / diag_name.str();

  svp::core::configure_memory_diagnostics(diag_path, diarize_memory_limit_bytes());

  if (!svp::audio::is_sherpa_diarization_available()) {
    std::cerr << "svp-builder diarize: sherpa-onnx not available\n";
    std::cerr << "  Library used: " << svp::audio::sherpa_lib_path_used() << "\n";
    std::cerr << "  Attempted paths:\n";
    for (const auto& p : svp::audio::sherpa_lib_paths_attempted()) {
      std::cerr << "    " << p << "\n";
    }
    return 1;
  }

  std::cerr << "svp-builder diarize: starting\n";
  std::cerr << "  wav: " << wav_path << "\n";
  std::cerr << "  model_dir: " << model_dir << "\n";
  std::cerr << "  lib: " << svp::audio::sherpa_lib_path_used() << "\n";
  std::cerr << "  diag: " << diag_path << "\n";

  svp::core::trace_memory_event("diarize.start", {
      {"wav", wav_path.string()},
      {"model_dir", model_dir.string()}
  });

  const auto t0 = std::chrono::steady_clock::now();

  svp::audio::SherpaDiarizationResult result =
      svp::audio::run_sherpa_diarization(wav_path, model_dir);

  const auto t1 = std::chrono::steady_clock::now();
  const auto elapsed_ms =
      std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();

  svp::core::trace_memory_event("diarize.end", {
      {"elapsed_ms", std::to_string(elapsed_ms)},
      {"ran", result.ran ? "true" : "false"},
      {"segment_count", std::to_string(result.segments.size())},
      {"final_speaker_count", std::to_string(result.final_speaker_count)}
  });

  if (!result.ran) {
    std::cerr << "svp-builder diarize: FAILED\n";
    for (const auto& b : result.blockers) {
      std::cerr << "  blocker: " << b << "\n";
    }
    return 1;
  }

  if (!options.segments_jsonl_path.empty()) {
    const fs::path segments_path(options.segments_jsonl_path);
    if (segments_path.has_parent_path()) {
      fs::create_directories(segments_path.parent_path());
    }
    std::ofstream segments_out(segments_path);
    if (!segments_out) {
      std::cerr << "svp-builder diarize: unable to write segments JSONL: "
                << segments_path << "\n";
      return 1;
    }
    for (std::size_t i = 0; i < result.segments.size(); ++i) {
      const auto& s = result.segments[i];
      segments_out << "{\"segment_index\":" << i
                   << ",\"start_us\":"
                   << static_cast<long long>(s.start_sec * 1000000.0f)
                   << ",\"end_us\":"
                   << static_cast<long long>(s.end_sec * 1000000.0f)
                   << ",\"speaker_id\":\"speaker_"
                   << std::setw(4) << std::setfill('0') << (s.speaker_id + 1)
                   << std::setfill(' ')
                   << "\"}\n";
    }
  }

  std::cout << "{\n";
  std::cout << "  \"ran\": true,\n";
  std::cout << "  \"elapsed_ms\": " << elapsed_ms << ",\n";
  std::cout << "  \"segment_count\": " << result.segments.size() << ",\n";
  std::cout << "  \"preliminary_segment_count\": "
            << result.preliminary_segments.size() << ",\n";
  std::cout << "  \"preliminary_cluster_count\": "
            << result.preliminary_cluster_count << ",\n";
  std::cout << "  \"final_speaker_count\": "
            << result.final_speaker_count << ",\n";
  std::cout << "  \"reconciliation_method\": \""
            << result.reconciliation_method << "\",\n";
  std::cout << "  \"diagnostics_log\": \""
            << diag_path.string() << "\"\n";
  std::cout << "}\n";

  if (!result.segments.empty()) {
    std::cerr << "  first 5 segments:\n";
    for (std::size_t i = 0;
         i < std::min<std::size_t>(5, result.segments.size()); ++i) {
      const auto& s = result.segments[i];
      std::cerr << "    [" << i << "] start=" << s.start_sec
                << " end=" << s.end_sec
                << " speaker=" << s.speaker_id << "\n";
    }
  }

  return 0;
}
