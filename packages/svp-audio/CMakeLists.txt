find_package(nlohmann_json CONFIG REQUIRED)

find_package(onnxruntime CONFIG QUIET)
if(onnxruntime_FOUND)
  set(SVP_AUDIO_ONNX_RUNTIME_AVAILABLE TRUE)
else()
  set(SVP_AUDIO_ONNX_RUNTIME_AVAILABLE FALSE)
endif()

add_library(svp-audio
  src/asr_chunk_planner.cpp
  src/asr_execution_boundary.cpp
  src/audio_extraction_executor.cpp
  src/audio_extraction_plan.cpp
  src/audio_stage_plan.cpp
  src/diarization_boundary.cpp
  src/sherpa_diarization/api.cpp
  src/sherpa_diarization/asr_word_assignment.cpp
  src/sherpa_diarization/embeddings.cpp
  src/sherpa_diarization/reconciliation.cpp
  src/sherpa_diarization/runner.cpp
  src/sherpa_diarization/sequence_decoder.cpp
  src/sherpa_diarization/wav_reader.cpp
  src/sherpa_diarization/windowing.cpp
  src/sherpa_diarization/word_assignment_replay.cpp
  src/sherpa_diarization/word_refinement.cpp
  src/transcript_records.cpp
  src/transcript_writer.cpp
  src/vad_execution_boundary.cpp
  src/vad_task_plan.cpp
  src/waveform_envelope.cpp
  src/whisper_mel.cpp
  src/whisper_model.cpp
  src/whisper_token_table.cpp
)

add_library(svp::audio ALIAS svp-audio)

target_include_directories(svp-audio
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_compile_features(svp-audio PUBLIC cxx_std_20)

if(SVP_AUDIO_ONNX_RUNTIME_AVAILABLE)
  target_compile_definitions(svp-audio PUBLIC SVP_AUDIO_ONNX_RUNTIME_AVAILABLE=1)
endif()

target_link_libraries(svp-audio
  PUBLIC
    nlohmann_json::nlohmann_json
    svp::media
    svp::models
)

if(BUILD_TESTING)
  add_executable(svp-audio-tests
    tests/audio_transcript_tests.cpp
  )

  target_compile_features(svp-audio-tests PRIVATE cxx_std_20)

  target_compile_definitions(svp-audio-tests
    PRIVATE
      SVP_REPO_ROOT="${CMAKE_SOURCE_DIR}"
  )

  target_link_libraries(svp-audio-tests
    PRIVATE
      svp::audio
  )

  add_test(NAME svp-audio-tests COMMAND svp-audio-tests)
endif()
