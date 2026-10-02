# Task-engine registration for svp-vision work (plan §4.2): the ocr.frame_batch
# task type, its parameter schema, its TaskSpec builder, and its output reader.
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
endif()
