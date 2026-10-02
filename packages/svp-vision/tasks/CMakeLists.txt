# Task-engine registration for svp-vision work (plan §4.2): the ocr.frame_batch
# task type and the dispatched stage work types (ocr.crop_batch,
# embed.text_batch, embed.keyframe_batch, depth.frame_batch), each with its
# parameter schema, TaskSpec builder, and output reader.
# svp-vision itself stays free of scheduler concerns; this thin layer is the
# only place vision code meets svp-exec, so a runtime that registers vision
# task types links svp::vision_tasks and nothing else in svp-vision changes.

find_package(OpenCV REQUIRED COMPONENTS core imgproc)

add_library(svp-vision-tasks
  src/ocr_calibration_clip.cpp
  src/ocr_frame_batch_parameters.cpp
  src/ocr_frame_batch_spec.cpp
  src/ocr_frame_batch_task.cpp
  src/ffmpeg_build_identity.cpp
  src/pp_ocr_session_pool.cpp
  src/pp_ocr_parameters.cpp
  src/parameter_fields.cpp
  src/item_batch_policy.cpp
  src/model_refs.cpp
  src/task_results.cpp
  src/task_spec_assembly.cpp
  src/onnx_model_pool.cpp
  src/onnx_model_parameters_json.cpp
  src/onnx_task_common.cpp
  src/onnx_task_registration.cpp
  src/dispatched_vision_tasks.cpp
  src/ocr_crop_batch_parameters.cpp
  src/ocr_crop_batch_spec.cpp
  src/ocr_crop_batch_task.cpp
  src/embed_text_batch_parameters.cpp
  src/embed_text_batch_spec.cpp
  src/embed_text_batch_execution.cpp
  src/embed_keyframe_batch_parameters.cpp
  src/embed_keyframe_batch_spec.cpp
  src/embed_keyframe_batch_execution.cpp
  src/depth_frame_batch_parameters.cpp
  src/depth_frame_batch_spec.cpp
  src/depth_frame_batch_execution.cpp
)

add_library(svp::vision_tasks ALIAS svp-vision-tasks)

target_include_directories(svp-vision-tasks
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_compile_features(svp-vision-tasks PUBLIC cxx_std_20)

target_link_libraries(svp-vision-tasks
  PUBLIC
    svp::exec
    svp::vision
  PRIVATE
    opencv_core
    opencv_imgproc
)

if(BUILD_TESTING)
  add_executable(svp-vision-ocr-frame-batch-task-tests
    tests/ocr_frame_batch_task_tests.cpp
  )
  target_link_libraries(svp-vision-ocr-frame-batch-task-tests
    PRIVATE
      svp::vision_tasks
  )
  add_test(NAME svp-vision-ocr-frame-batch-task-tests
           COMMAND svp-vision-ocr-frame-batch-task-tests)

  add_executable(svp-vision-dispatched-task-spec-tests
    tests/dispatched_task_spec_tests.cpp
  )
  target_link_libraries(svp-vision-dispatched-task-spec-tests
    PRIVATE
      svp::vision_tasks
  )
  add_test(NAME svp-vision-dispatched-task-spec-tests
           COMMAND svp-vision-dispatched-task-spec-tests)

  # A worker process serving ocr.frame_batch on stdin/stdout for the loopback
  # executor, with inputs and outputs in a content-addressed cache.
  add_executable(svp-vision-ocr-task-test-worker
    tests/ocr_task_test_worker_main.cpp
  )
  target_link_libraries(svp-vision-ocr-task-test-worker
    PRIVATE
      svp::vision_tasks
  )

  # Real-model check that ocr.frame_batch output is byte-identical in-process,
  # through the in-process executor, and through the loopback executor's child
  # process. Needs a source video, the PP-OCR models, and ffmpeg, named by
  # SVP_OCR_TASK_TEST_* environment variables; skips without them.
  add_executable(svp-vision-ocr-frame-batch-loopback-tests
    tests/ocr_frame_batch_loopback_tests.cpp
  )
  target_link_libraries(svp-vision-ocr-frame-batch-loopback-tests
    PRIVATE
      svp::vision_tasks
  )
  add_dependencies(svp-vision-ocr-frame-batch-loopback-tests
    svp-vision-ocr-task-test-worker
  )
  add_test(NAME svp-vision-ocr-frame-batch-loopback-tests
           COMMAND svp-vision-ocr-frame-batch-loopback-tests
                   $<TARGET_FILE:svp-vision-ocr-task-test-worker>)
  set_tests_properties(svp-vision-ocr-frame-batch-loopback-tests PROPERTIES
    SKIP_REGULAR_EXPRESSION "^Skipping:")

  # Real-model checks that every dispatched vision task type computes the
  # stage's own per-item work (direct, in-process executor, loopback child),
  # and that each stage given a dispatcher writes the staging it writes
  # without one. Needs SVP_MODEL_CACHE_ROOT and ffmpeg; skips without them.
  add_executable(svp-vision-dispatched-work-tests
    tests/dispatched_vision_work_tests.cpp
  )
  target_include_directories(svp-vision-dispatched-work-tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../src
  )
  target_link_libraries(svp-vision-dispatched-work-tests
    PRIVATE
      svp::vision_tasks
  )
  add_dependencies(svp-vision-dispatched-work-tests
    svp-vision-ocr-task-test-worker
  )
  add_test(NAME svp-vision-dispatched-work-tests
           COMMAND svp-vision-dispatched-work-tests
                   $<TARGET_FILE:svp-vision-ocr-task-test-worker>)
  set_tests_properties(svp-vision-dispatched-work-tests PROPERTIES
    SKIP_REGULAR_EXPRESSION "^Skipping:")
endif()
