# Task-engine registration for svp-audio work (plan §4.2, M5): asr.chunk_batch
# (ASR chunks on the fixed chunk grid) and diarize.window (the per-window map
# of sherpa-onnx diarization), each with its parameter schema, TaskSpec
# builder, and output reader. svp-audio itself stays free of scheduler
# concerns; this layer is the only place audio code meets svp-exec.

add_library(svp-audio-tasks
  src/asr_chunk_batch_parameters.cpp
  src/asr_chunk_batch_spec.cpp
  src/asr_chunk_batch_task.cpp
  src/audio_task_registration.cpp
  src/audio_task_spec.cpp
  src/diarize_window_spec.cpp
  src/diarize_window_task.cpp
  src/task_outputs.cpp
)

add_library(svp::audio_tasks ALIAS svp-audio-tasks)

target_include_directories(svp-audio-tasks
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_compile_features(svp-audio-tasks PUBLIC cxx_std_20)

# Batch sizing reuses the dispatched vision work's item batch policy
# (svp/vision/tasks/item_batch_policy.hpp).
target_link_libraries(svp-audio-tasks
  PUBLIC
    svp::audio
    svp::exec
    svp::vision_tasks
)

if(BUILD_TESTING)
  add_executable(svp-audio-task-tests
    tests/audio_task_tests.cpp
  )
  target_compile_features(svp-audio-task-tests PRIVATE cxx_std_20)
  target_compile_options(svp-audio-task-tests PRIVATE -UNDEBUG)
  target_compile_definitions(svp-audio-task-tests
    PRIVATE
      SVP_REPO_ROOT="${CMAKE_SOURCE_DIR}"
  )
  target_link_libraries(svp-audio-task-tests PRIVATE svp::audio_tasks)
  add_test(NAME svp-audio-task-tests COMMAND svp-audio-task-tests)
endif()
