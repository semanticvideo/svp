#include "audio_test_support.hpp"

void test_whisper_runtime_available_reports_honestly() {
  const bool available = svp::audio::is_whisper_runtime_available();
#if defined(SVP_AUDIO_WHISPER_CPP_AVAILABLE)
  assert(available);
#else
  assert(!available);
#endif
}

void test_whisper_inference_blocks_when_model_dir_missing() {
  const std::filesystem::path fake_dir =
      std::filesystem::temp_directory_path() / "svp-whisper-fake-model";
  std::filesystem::remove_all(fake_dir);
  std::filesystem::create_directories(fake_dir);

  const std::filesystem::path fake_wav = fake_dir / "test.wav";
  {
    std::ofstream output(fake_wav, std::ios::binary);
    write_u32_le(output, 0x46464952);
    write_u32_le(output, 36 + 16000 * 2);
    output.write("WAVE", 4);
    output.write("fmt ", 4);
    write_u32_le(output, 16);
    write_u16_le(output, 1);
    write_u16_le(output, 1);
    write_u32_le(output, 16000);
    write_u32_le(output, 32000);
    write_u16_le(output, 2);
    write_u16_le(output, 16);
    output.write("data", 4);
    write_u32_le(output, 16000 * 2);
    for (int i = 0; i < 16000; ++i) {
      write_u16_le(output, 0);
    }
  }

  const svp::audio::WhisperInferenceResult result =
      svp::audio::run_whisper_inference(fake_wav, fake_dir, "test_chunk", 0,
                                        1000000);

  assert(!result.ran);
  assert(!result.blockers.empty());

  std::filesystem::remove_all(fake_dir);
}
