find_package(nlohmann_json CONFIG REQUIRED)
find_package(OpenCV REQUIRED COMPONENTS core imgproc video calib3d)

add_library(svp-vision
  src/canonical_frame_input.cpp
  src/color_frame_sampling.cpp
  src/frame_catalog.cpp
  src/color_observation_records.cpp
  src/color_quantization.cpp
  src/depth_generation.cpp
  src/embedding_generation.cpp
  src/evidence_crop.cpp
  src/foundation_color_observations.cpp
  src/foundation_color_staging.cpp
  src/foundation_ocr_staging.cpp
  src/observation_pipeline_plan.cpp
  src/ocr_generation/common.cpp
  src/ocr_generation/crop_hardening.cpp
  src/ocr_generation/numeric_parser.cpp
  src/ocr_generation/reconciliation.cpp
  src/ocr_generation/record_emission.cpp
  src/ocr_generation/result_json.cpp
  src/ocr_generation/staging_io.cpp
  src/ocr_generation.cpp
  src/ocr_temporal_sampling.cpp
  src/pp_ocr/detection.cpp
  src/pp_ocr/model_bundles.cpp
  src/pp_ocr/recognition.cpp
  src/pp_ocr.cpp
  src/real_frame_color_sampling.cpp
  src/scene_segmentation.cpp
  src/text_tokenizer.cpp
  src/visual_entity_pipeline.cpp
  src/visual_entity_depth_proposals.cpp
  src/visual_entity_depth_schedule.cpp
  src/visual_entity_cut_detection.cpp
  src/visual_entity_frame_decoder.cpp
  src/visual_entity_detector.cpp
  src/visual_entity_motion_policy.cpp
  src/visual_entity_sampling.cpp
  src/visual_entity_tracker.cpp
  src/visual_entity_window_assembler.cpp
  src/mask_writer.cpp
  src/noise_suppression.cpp
)

add_library(svp::vision ALIAS svp-vision)

target_include_directories(svp-vision
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_compile_features(svp-vision PUBLIC cxx_std_20)

target_link_libraries(svp-vision
  PUBLIC
    nlohmann_json::nlohmann_json
    svp::blocks
    svp::media
    svp::models
  PRIVATE
    opencv_core
    opencv_imgproc
    # vcpkg's opencv_video target pulls opencv_dnn into this static binary,
    # which conflicts with ONNX Runtime's schema registry during OCR model load.
    $<TARGET_FILE:opencv_video>
    opencv_calib3d
)

add_executable(svp-visual-entity-diagnostic
  tools/visual_entity_diagnostic.cpp
)

add_executable(svp-visual-entity-fixture-score
  tools/visual_entity_fixture_score.cpp
)

add_executable(svp-visual-entity-detector-diagnostic
  tools/visual_entity_detector_diagnostic.cpp
)

target_compile_features(svp-visual-entity-detector-diagnostic PRIVATE cxx_std_20)

target_link_libraries(svp-visual-entity-detector-diagnostic
  PRIVATE
    svp::vision
)

target_compile_features(svp-visual-entity-fixture-score PRIVATE cxx_std_20)

target_link_libraries(svp-visual-entity-fixture-score
  PRIVATE
    nlohmann_json::nlohmann_json
)

target_compile_features(svp-visual-entity-diagnostic PRIVATE cxx_std_20)

target_link_libraries(svp-visual-entity-diagnostic
  PRIVATE
    svp::vision
)

if(BUILD_TESTING)
  add_executable(svp-vision-visual-entity-sampling-tests
    tests/visual_entity_sampling_tests.cpp
  )

  target_compile_features(svp-vision-visual-entity-sampling-tests PRIVATE
    cxx_std_20
  )

  target_link_libraries(svp-vision-visual-entity-sampling-tests
    PRIVATE
      svp::vision
  )

  add_test(NAME svp-vision-visual-entity-sampling-tests
           COMMAND svp-vision-visual-entity-sampling-tests)

  add_executable(svp-vision-visual-entity-window-assembler-tests
    tests/visual_entity_window_assembler_tests.cpp
  )

  target_compile_features(svp-vision-visual-entity-window-assembler-tests
    PRIVATE
      cxx_std_20
  )

  target_link_libraries(svp-vision-visual-entity-window-assembler-tests
    PRIVATE
      svp::vision
  )

  add_test(NAME svp-vision-visual-entity-window-assembler-tests
           COMMAND svp-vision-visual-entity-window-assembler-tests)

  add_executable(svp-vision-visual-entity-depth-proposal-tests
    tests/visual_entity_depth_proposal_tests.cpp
  )

  target_compile_features(svp-vision-visual-entity-depth-proposal-tests PRIVATE
    cxx_std_20
  )

  target_link_libraries(svp-vision-visual-entity-depth-proposal-tests
    PRIVATE
      svp::vision
      opencv_core
  )

  add_test(NAME svp-vision-visual-entity-depth-proposal-tests
           COMMAND svp-vision-visual-entity-depth-proposal-tests)

  add_executable(svp-vision-visual-entity-motion-policy-tests
    tests/visual_entity_motion_policy_tests.cpp
  )

  target_compile_features(svp-vision-visual-entity-motion-policy-tests PRIVATE
    cxx_std_20
  )

  target_link_libraries(svp-vision-visual-entity-motion-policy-tests
    PRIVATE
      svp::vision
      opencv_core
  )

  add_test(NAME svp-vision-visual-entity-motion-policy-tests
           COMMAND svp-vision-visual-entity-motion-policy-tests)

  add_executable(svp-vision-visual-entity-depth-schedule-tests
    tests/visual_entity_depth_schedule_tests.cpp
  )

  target_compile_features(svp-vision-visual-entity-depth-schedule-tests PRIVATE
    cxx_std_20
  )

  target_link_libraries(svp-vision-visual-entity-depth-schedule-tests
    PRIVATE
      svp::vision
  )

  add_test(NAME svp-vision-visual-entity-depth-schedule-tests
           COMMAND svp-vision-visual-entity-depth-schedule-tests)

  add_executable(svp-vision-visual-entity-cut-detection-tests
    tests/visual_entity_cut_detection_tests.cpp
  )
  target_link_libraries(svp-vision-visual-entity-cut-detection-tests
    PRIVATE svp::vision)
  add_test(NAME svp-vision-visual-entity-cut-detection-tests
           COMMAND svp-vision-visual-entity-cut-detection-tests)

  add_executable(svp-vision-plan-tests
    tests/observation_pipeline_plan_tests.cpp
  )

  target_compile_features(svp-vision-plan-tests PRIVATE cxx_std_20)

  target_link_libraries(svp-vision-plan-tests
    PRIVATE
      svp::vision
  )

  add_test(NAME svp-vision-plan-tests COMMAND svp-vision-plan-tests)

  add_executable(svp-vision-color-quantization-tests
    tests/color_quantization_tests.cpp
  )

  target_compile_features(svp-vision-color-quantization-tests PRIVATE cxx_std_20)

  target_link_libraries(svp-vision-color-quantization-tests
    PRIVATE
      svp::vision
  )

  add_test(NAME svp-vision-color-quantization-tests
           COMMAND svp-vision-color-quantization-tests)

  add_executable(svp-vision-color-observation-records-tests
    tests/color_observation_records_tests.cpp
  )

  target_compile_features(svp-vision-color-observation-records-tests PRIVATE cxx_std_20)

  target_link_libraries(svp-vision-color-observation-records-tests
    PRIVATE
      svp::vision
  )

  add_test(NAME svp-vision-color-observation-records-tests
           COMMAND svp-vision-color-observation-records-tests)

  add_executable(svp-vision-color-frame-sampling-tests
    tests/color_frame_sampling_tests.cpp
  )

  target_compile_features(svp-vision-color-frame-sampling-tests PRIVATE cxx_std_20)

  target_link_libraries(svp-vision-color-frame-sampling-tests
    PRIVATE
      svp::vision
  )

  add_test(NAME svp-vision-color-frame-sampling-tests
           COMMAND svp-vision-color-frame-sampling-tests)

  add_executable(svp-vision-foundation-color-observations-tests
    tests/foundation_color_observations_tests.cpp
  )

  target_compile_features(svp-vision-foundation-color-observations-tests
    PRIVATE
      cxx_std_20
  )

  target_link_libraries(svp-vision-foundation-color-observations-tests
    PRIVATE
      svp::vision
  )

  add_test(NAME svp-vision-foundation-color-observations-tests
           COMMAND svp-vision-foundation-color-observations-tests)

  add_executable(svp-vision-foundation-color-staging-tests
    tests/foundation_color_staging_tests.cpp
  )

  target_compile_features(svp-vision-foundation-color-staging-tests
    PRIVATE
      cxx_std_20
  )

  target_link_libraries(svp-vision-foundation-color-staging-tests
    PRIVATE
      svp::vision
  )

  add_test(NAME svp-vision-foundation-color-staging-tests
           COMMAND svp-vision-foundation-color-staging-tests)

  add_executable(svp-vision-real-frame-color-sampling-tests
    tests/real_frame_color_sampling_tests.cpp
  )

  target_compile_features(svp-vision-real-frame-color-sampling-tests
    PRIVATE
      cxx_std_20
  )

  target_link_libraries(svp-vision-real-frame-color-sampling-tests
    PRIVATE
      svp::vision
  )

  add_test(NAME svp-vision-real-frame-color-sampling-tests
           COMMAND svp-vision-real-frame-color-sampling-tests)

  add_executable(svp-vision-scene-segmentation-tests
    tests/scene_segmentation_tests.cpp
  )

  target_compile_features(svp-vision-scene-segmentation-tests PRIVATE
    cxx_std_20
  )

  target_link_libraries(svp-vision-scene-segmentation-tests
    PRIVATE
      svp::vision
  )

  add_test(NAME svp-vision-scene-segmentation-tests
           COMMAND svp-vision-scene-segmentation-tests)

  add_executable(svp-vision-foundation-ocr-staging-tests
    tests/foundation_ocr_staging_tests.cpp
  )

  target_compile_features(svp-vision-foundation-ocr-staging-tests
    PRIVATE
      cxx_std_20
  )

  target_link_libraries(svp-vision-foundation-ocr-staging-tests
    PRIVATE
      svp::vision
  )

  add_test(NAME svp-vision-foundation-ocr-staging-tests
           COMMAND svp-vision-foundation-ocr-staging-tests)

  add_executable(svp-vision-depth-generation-boundary-tests
    tests/depth_generation_boundary_tests.cpp
  )

  target_compile_features(svp-vision-depth-generation-boundary-tests PRIVATE
    cxx_std_20
  )

  target_link_libraries(svp-vision-depth-generation-boundary-tests
    PRIVATE
      svp::vision
  )

  add_test(NAME svp-vision-depth-generation-boundary-tests
           COMMAND svp-vision-depth-generation-boundary-tests)

  add_executable(svp-vision-embedding-generation-smoke-test
    tests/embedding_generation_smoke_test.cpp
  )

  target_compile_features(svp-vision-embedding-generation-smoke-test PRIVATE
    cxx_std_20
  )

  target_link_libraries(svp-vision-embedding-generation-smoke-test
    PRIVATE
      svp::vision
  )

  add_test(NAME svp-vision-embedding-generation-smoke-test
           COMMAND svp-vision-embedding-generation-smoke-test)

  add_executable(svp-vision-evidence-crop-linking-tests
    tests/evidence_crop_linking_tests.cpp
  )

  target_compile_features(svp-vision-evidence-crop-linking-tests PRIVATE
    cxx_std_20
  )

  target_link_libraries(svp-vision-evidence-crop-linking-tests
    PRIVATE
      svp::vision
  )

  add_test(NAME svp-vision-evidence-crop-linking-tests
           COMMAND svp-vision-evidence-crop-linking-tests)

  add_executable(svp-vision-ocr-temporal-sampling-tests
    tests/ocr_temporal_sampling_tests.cpp
  )

  target_compile_features(svp-vision-ocr-temporal-sampling-tests PRIVATE
    cxx_std_20
  )

  target_link_libraries(svp-vision-ocr-temporal-sampling-tests
    PRIVATE
      svp::vision
  )

  add_test(NAME svp-vision-ocr-temporal-sampling-tests
           COMMAND svp-vision-ocr-temporal-sampling-tests)

  add_executable(svp-vision-ocr-numeric-parser-tests
    tests/ocr_numeric_parser_tests.cpp
  )

  target_compile_features(svp-vision-ocr-numeric-parser-tests PRIVATE
    cxx_std_20
  )

  target_link_libraries(svp-vision-ocr-numeric-parser-tests
    PRIVATE
      svp::vision
  )

  add_test(NAME svp-vision-ocr-numeric-parser-tests
           COMMAND svp-vision-ocr-numeric-parser-tests)

  add_executable(svp-vision-visual-entity-tracker-tests
    tests/visual_entity_tracker_tests.cpp
  )

  target_compile_features(svp-vision-visual-entity-tracker-tests PRIVATE
    cxx_std_20
  )

  target_link_libraries(svp-vision-visual-entity-tracker-tests
    PRIVATE
      svp::vision
  )

  add_test(NAME svp-vision-visual-entity-tracker-tests
           COMMAND svp-vision-visual-entity-tracker-tests)

  add_executable(svp-vision-progress-callback-tests
    tests/progress_callback_tests.cpp
  )

  target_compile_features(svp-vision-progress-callback-tests PRIVATE
    cxx_std_20
  )

  target_link_libraries(svp-vision-progress-callback-tests
    PRIVATE
      svp::vision
  )

  add_test(NAME svp-vision-progress-callback-tests
           COMMAND svp-vision-progress-callback-tests)

  add_executable(svp-vision-pp-ocr-recognition-tests
    tests/pp_ocr_recognition_tests.cpp
  )

  target_compile_features(svp-vision-pp-ocr-recognition-tests PRIVATE
    cxx_std_20
  )

  target_link_libraries(svp-vision-pp-ocr-recognition-tests
    PRIVATE
      svp::vision
  )

  add_test(NAME svp-vision-pp-ocr-recognition-tests
           COMMAND svp-vision-pp-ocr-recognition-tests)
endif()
