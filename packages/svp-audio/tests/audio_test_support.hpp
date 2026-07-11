#pragma once

#include "svp/audio/asr_chunk_planner.hpp"
#include "svp/audio/asr_execution_boundary.hpp"
#include "svp/audio/audio_extraction_executor.hpp"
#include "svp/audio/audio_stage_plan.hpp"
#include "svp/audio/diarization_boundary.hpp"
#include "svp/audio/sherpa_diarization.hpp"
#include "svp/audio/transcript_records.hpp"
#include "svp/audio/transcript_writer.hpp"
#include "svp/audio/vad_execution_boundary.hpp"
#include "svp/audio/vad_task_plan.hpp"
#include "svp/audio/waveform_envelope.hpp"
#include "svp/audio/whisper_model.hpp"
#include "../src/sherpa_diarization/private.hpp"

#include <nlohmann/json.hpp>

#include <cassert>
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

inline void write_u16_le(std::ostream& output, std::uint16_t value) {
  output.put(static_cast<char>(value & 0xff));
  output.put(static_cast<char>((value >> 8) & 0xff));
}

inline void write_u32_le(std::ostream& output, std::uint32_t value) {
  output.put(static_cast<char>(value & 0xff));
  output.put(static_cast<char>((value >> 8) & 0xff));
  output.put(static_cast<char>((value >> 16) & 0xff));
  output.put(static_cast<char>((value >> 24) & 0xff));
}

inline void write_pcm_s16le_mono_wav(const std::filesystem::path& path,
                                     const std::vector<std::int16_t>& samples) {
  std::ofstream output(path, std::ios::binary);
  const std::uint32_t data_size = static_cast<std::uint32_t>(samples.size() * 2);
  output.write("RIFF", 4);
  write_u32_le(output, 36 + data_size);
  output.write("WAVE", 4);
  output.write("fmt ", 4);
  write_u32_le(output, 16);
  write_u16_le(output, 1);
  write_u16_le(output, 1);
  write_u32_le(output, 16000);
  write_u32_le(output, 16000 * 2);
  write_u16_le(output, 2);
  write_u16_le(output, 16);
  output.write("data", 4);
  write_u32_le(output, data_size);
  for (const std::int16_t sample : samples) {
    write_u16_le(output, static_cast<std::uint16_t>(sample));
  }
}

inline svp::audio::SherpaDiarizationSegment test_diarization_segment(
    float start_sec,
    float duration_sec,
    int32_t speaker_id) {
  return {start_sec, start_sec + duration_sec, speaker_id};
}
