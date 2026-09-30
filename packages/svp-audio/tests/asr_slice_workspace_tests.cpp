#include "audio_test_support.hpp"
#include "svp/audio/asr_slice_workspace.hpp"
#include "svp/audio/wav_slice.hpp"

#include <array>
#include <iterator>
#include <thread>

#include <unistd.h>

namespace {

std::filesystem::path fresh_test_parent(const std::string& name) {
  const std::filesystem::path parent =
      std::filesystem::temp_directory_path() /
      (name + "-" + std::to_string(::getpid()));
  std::filesystem::remove_all(parent);
  std::filesystem::create_directories(parent);
  return parent;
}

std::string read_bytes(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(input),
          std::istreambuf_iterator<char>()};
}

std::vector<std::int16_t> ramp_samples(std::size_t count) {
  std::vector<std::int16_t> samples(count);
  for (std::size_t index = 0; index < count; ++index) {
    samples[index] = static_cast<std::int16_t>(index % 2000);
  }
  return samples;
}

}  // namespace

void test_asr_slice_workspaces_are_unique_and_removed() {
  const std::filesystem::path parent =
      fresh_test_parent("svp-asr-slice-workspace-unique-test");
  std::filesystem::path first_path;
  std::filesystem::path second_path;
  {
    const svp::audio::AsrSliceWorkspace first(parent);
    const svp::audio::AsrSliceWorkspace second(parent);
    first_path = first.path();
    second_path = second.path();
    assert(first_path != second_path);
    assert(std::filesystem::is_directory(first_path));
    assert(std::filesystem::is_directory(second_path));
    assert(first_path.parent_path() == parent);
    assert(first_path.filename().string().starts_with(
        svp::audio::kAsrSliceWorkspacePrefix));

    // Leftover slices must not survive the workspace.
    std::ofstream(first_path / "leftover.wav") << "x";
  }
  assert(!std::filesystem::exists(first_path));
  assert(!std::filesystem::exists(second_path));
  std::filesystem::remove_all(parent);
}

void test_concurrent_asr_slice_workspaces_do_not_share_slices() {
  // Two boundary executions slicing the same chunk window at the same time
  // (the concurrent-builds case) must write to distinct files with identical
  // bytes, and neither may observe the other's cleanup.
  const std::filesystem::path parent =
      fresh_test_parent("svp-asr-slice-workspace-concurrent-test");
  const std::filesystem::path input_wav = parent / "analysis.wav";
  constexpr std::size_t kInputSampleCount = 16000 * 3;
  write_pcm_s16le_mono_wav(input_wav, ramp_samples(kInputSampleCount));
  constexpr std::int64_t kSliceStartUs = 500000;
  constexpr std::int64_t kSliceEndUs = 2500000;
  constexpr std::size_t kExecutions = 2;
  constexpr int kRepeatsPerExecution = 20;

  std::array<std::filesystem::path, kExecutions> workspace_paths;
  std::array<std::string, kExecutions> slice_bytes;
  std::array<bool, kExecutions> every_slice_matched{};
  std::vector<std::thread> threads;
  for (std::size_t execution = 0; execution < kExecutions; ++execution) {
    threads.emplace_back([&, execution]() {
      const svp::audio::AsrSliceWorkspace workspace(parent);
      workspace_paths[execution] = workspace.path();
      bool matched = true;
      for (int repeat = 0; repeat < kRepeatsPerExecution; ++repeat) {
        const std::filesystem::path slice = svp::audio::slice_wav_to_temp(
            input_wav, kSliceStartUs, kSliceEndUs, workspace.path());
        const std::string bytes = read_bytes(slice);
        if (repeat == 0) slice_bytes[execution] = bytes;
        matched = matched && bytes == slice_bytes[execution];
        std::filesystem::remove(slice);
      }
      every_slice_matched[execution] = matched;
    });
  }
  for (std::thread& thread : threads) thread.join();

  assert(workspace_paths[0] != workspace_paths[1]);
  assert(every_slice_matched[0] && every_slice_matched[1]);
  assert(!slice_bytes[0].empty());
  assert(slice_bytes[0] == slice_bytes[1]);
  assert(!std::filesystem::exists(workspace_paths[0]));
  assert(!std::filesystem::exists(workspace_paths[1]));

  // Slice contents are independent of the workspace location.
  const std::filesystem::path legacy_dir = parent / "legacy-shared-dir";
  const std::filesystem::path legacy_slice = svp::audio::slice_wav_to_temp(
      input_wav, kSliceStartUs, kSliceEndUs, legacy_dir);
  assert(read_bytes(legacy_slice) == slice_bytes[0]);
  std::filesystem::remove_all(parent);
}
