#include "private.hpp"

#include <cstdlib>
#include <dlfcn.h>
#include <filesystem>
#include <type_traits>

namespace svp::audio {
namespace sherpa_diarization_internal {

SherpaLibState& lib_state() {
  static SherpaLibState state;
  return state;
}

void add_sherpa_lib_from_dir(std::vector<std::string>& candidates,
                              const std::filesystem::path& lib_dir,
                              const std::string& filename) {
  if (!std::filesystem::exists(lib_dir)) return;
  for (const auto& entry : std::filesystem::directory_iterator(lib_dir)) {
    if (!entry.is_directory()) continue;
    const std::string name = entry.path().filename().string();
    if (name.find("python3.") == std::string::npos) continue;
    // Check both site-packages and dist-packages layouts
    for (const auto& pkg_dir : {"site-packages", "dist-packages"}) {
      const std::filesystem::path sherpa_lib =
          entry.path() / pkg_dir / "sherpa_onnx" / "lib" / filename;
      if (std::filesystem::exists(sherpa_lib)) {
        candidates.push_back(sherpa_lib.string());
      }
    }
  }
}

void add_sherpa_lib_from_env(std::vector<std::string>& candidates,
                              const char* env_var,
                              const std::string& filename) {
  const char* env_val = std::getenv(env_var);
  if (!env_val || !env_val[0]) return;
  std::filesystem::path env_path(env_val);
  // venv/Conda lib directories: <env>/lib/python3.*/site-packages/sherpa_onnx/lib/
  add_sherpa_lib_from_dir(candidates, env_path / "lib", filename);
  // Also check <env>/lib/sherpa_onnx/ (some installs place libs directly)
  const std::filesystem::path direct_lib = env_path / "lib" / "sherpa_onnx" / "lib" / filename;
  if (std::filesystem::exists(direct_lib)) {
    candidates.push_back(direct_lib.string());
  }
  // Conda sometimes places libs in <env>/lib/ directly
  const std::filesystem::path conda_lib = env_path / "lib" / filename;
  if (std::filesystem::exists(conda_lib)) {
    candidates.push_back(conda_lib.string());
  }
}

std::vector<std::string> build_candidate_paths() {
  std::vector<std::string> candidates;

  // 1. Explicit path set via set_sherpa_lib_path()
  if (!lib_state().explicit_path.empty()) {
    candidates.push_back(lib_state().explicit_path);
  }

  // 2. SHERPA_ONNX_LIB_PATH env var
  const char* env_path = std::getenv("SHERPA_ONNX_LIB_PATH");
  if (env_path && env_path[0]) {
    candidates.push_back(env_path);
  }

  // 3. macOS user site-packages: ~/Library/Python/3.{9..14}/lib/python/site-packages/
  const char* home = std::getenv("HOME");
  if (home && home[0]) {
    std::string home_str(home);
    for (int minor = 9; minor <= 14; ++minor) {
      candidates.push_back(home_str +
          "/Library/Python/3." + std::to_string(minor) +
          "/lib/python/site-packages/sherpa_onnx/lib/libsherpa-onnx-c-api.dylib");
    }
    // Linux pip --user: ~/.local/lib/python3.*/site-packages/ and dist-packages/
    add_sherpa_lib_from_dir(candidates,
        std::filesystem::path(home_str) / ".local" / "lib",
        "libsherpa-onnx-c-api.so");
  }

  // 4. Virtual environments (venv, Conda)
  add_sherpa_lib_from_env(candidates, "VIRTUAL_ENV", "libsherpa-onnx-c-api.dylib");
  add_sherpa_lib_from_env(candidates, "CONDA_PREFIX", "libsherpa-onnx-c-api.dylib");
  // On Linux venvs the shared lib is .so
  add_sherpa_lib_from_env(candidates, "VIRTUAL_ENV", "libsherpa-onnx-c-api.so");
  add_sherpa_lib_from_env(candidates, "CONDA_PREFIX", "libsherpa-onnx-c-api.so");

  // 5. Homebrew site-packages (Apple Silicon and Intel)
  add_sherpa_lib_from_dir(candidates, "/opt/homebrew/lib", "libsherpa-onnx-c-api.dylib");
  add_sherpa_lib_from_dir(candidates, "/usr/local/lib", "libsherpa-onnx-c-api.dylib");

  // 6. Direct Homebrew and system library paths (macOS)
  candidates.push_back("/opt/homebrew/lib/libsherpa-onnx-c-api.dylib");
  candidates.push_back("/usr/local/lib/libsherpa-onnx-c-api.dylib");

  // 7. Linux system paths (site-packages and dist-packages scanned above via HOME)
  //    Also check common system-level Python directories
  add_sherpa_lib_from_dir(candidates, "/usr/lib", "libsherpa-onnx-c-api.so");
  add_sherpa_lib_from_dir(candidates, "/usr/local/lib", "libsherpa-onnx-c-api.so");
  candidates.push_back("/usr/local/lib/libsherpa-onnx-c-api.so");
  candidates.push_back("/usr/lib/libsherpa-onnx-c-api.so");
  candidates.push_back("/usr/lib/x86_64-linux-gnu/libsherpa-onnx-c-api.so");
  candidates.push_back("/usr/lib/aarch64-linux-gnu/libsherpa-onnx-c-api.so");

  return candidates;
}

SherpaDiarizationApi& get_api() {
  static SherpaDiarizationApi api;
  if (api.loaded) return api;
  api.loaded = true;

  std::vector<std::string> candidates = build_candidate_paths();
  lib_state().attempted_paths = candidates;

  for (const auto& path : candidates) {
    api.lib_handle = dlopen(path.c_str(), RTLD_LAZY | RTLD_LOCAL);
    if (api.lib_handle) {
      lib_state().loaded_path = path;
      break;
    }
  }

  if (!api.lib_handle) {
    return api;
  }

  auto load = [&](auto& fn, const char* name) {
    fn = reinterpret_cast<std::remove_reference_t<decltype(fn)>>(dlsym(api.lib_handle, name));
  };

  load(api.create, "SherpaOnnxCreateOfflineSpeakerDiarization");
  load(api.destroy, "SherpaOnnxDestroyOfflineSpeakerDiarization");
  load(api.process, "SherpaOnnxOfflineSpeakerDiarizationProcess");
  load(api.destroy_result, "SherpaOnnxOfflineSpeakerDiarizationDestroyResult");
  load(api.get_num_speakers, "SherpaOnnxOfflineSpeakerDiarizationResultGetNumSpeakers");
  load(api.get_num_segments, "SherpaOnnxOfflineSpeakerDiarizationResultGetNumSegments");
  load(api.sort_by_start_time, "SherpaOnnxOfflineSpeakerDiarizationResultSortByStartTime");
  load(api.destroy_segment, "SherpaOnnxOfflineSpeakerDiarizationDestroySegment");

  load(api.emb_create, "SherpaOnnxCreateSpeakerEmbeddingExtractor");
  load(api.emb_destroy, "SherpaOnnxDestroySpeakerEmbeddingExtractor");
  load(api.emb_dim, "SherpaOnnxSpeakerEmbeddingExtractorDim");
  load(api.emb_create_stream, "SherpaOnnxSpeakerEmbeddingExtractorCreateStream");
  load(api.stream_accept, "SherpaOnnxOnlineStreamAcceptWaveform");
  load(api.stream_input_finished, "SherpaOnnxOnlineStreamInputFinished");
  load(api.emb_is_ready, "SherpaOnnxSpeakerEmbeddingExtractorIsReady");
  load(api.emb_compute, "SherpaOnnxSpeakerEmbeddingExtractorComputeEmbedding");
  load(api.emb_destroy_vec, "SherpaOnnxSpeakerEmbeddingExtractorDestroyEmbedding");
  load(api.stream_destroy, "SherpaOnnxDestroyOnlineStream");

  if (!api.create || !api.destroy || !api.process || !api.destroy_result ||
      !api.get_num_speakers || !api.get_num_segments || !api.sort_by_start_time || !api.destroy_segment) {
    api = SherpaDiarizationApi{};
    api.loaded = true;
  }

  return api;
}


}  // namespace sherpa_diarization_internal

void set_sherpa_lib_path(const std::string& path) {
  sherpa_diarization_internal::lib_state().explicit_path = path;
}

std::string sherpa_lib_path_used() {
  return sherpa_diarization_internal::lib_state().loaded_path;
}

std::vector<std::string> sherpa_lib_paths_attempted() {
  return sherpa_diarization_internal::lib_state().attempted_paths;
}

bool is_sherpa_diarization_available() {
  const sherpa_diarization_internal::SherpaDiarizationApi& api =
      sherpa_diarization_internal::get_api();
  return api.lib_handle != nullptr && api.create != nullptr;
}

}  // namespace svp::audio
