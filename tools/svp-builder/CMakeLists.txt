find_package(CLI11 CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(libzip CONFIG REQUIRED)

add_library(svp-builder-lib
  src/audio_stage.cpp
  src/build_frame_plan.cpp
  src/build_pipeline.cpp
  src/build_pipeline_io.cpp
  src/build_progress.cpp
  src/build_progress_catalog.cpp
  src/build_run_report_json.cpp
  src/build_run_report_recorder.cpp
  src/build_stage_metadata.cpp
  src/build_thread_plan.cpp
  src/foundation_color_stage.cpp
  src/foundation_ocr_stage.cpp
  src/microphone_asr_stage.cpp
  src/model_cache_preflight.cpp
  src/package_skeleton_stage.cpp
  src/process_resources.cpp
  src/progress_renderer.cpp
  src/progress_timeline.cpp
  src/runtime_tools.cpp
  src/runtime_tools_json.cpp
  src/vision_plan_stage.cpp
  src/interlace/interlace_create.cpp
  src/interlace/interlace_validate.cpp
  src/interlace/interlace_inspect.cpp
  src/interlace/interlace_extract.cpp
  src/interlace/interlace_recombine.cpp
  src/transport/embedded_transport_embed.cpp
  src/transport/embedded_transport_build.cpp
  src/transport/embedded_transport_extract.cpp
  src/transport/embedded_transport_strip.cpp
  src/transport/replacement_binding_media.cpp
  src/transport/transport_output_policy.cpp
  src/interlace/interlace_batch_discovery.cpp
  src/interlace/interlace_batch_sidecar.cpp
  src/interlace/interlace_batch_output.cpp
  src/interlace/interlace_batch_embedded.cpp
  src/interlace/interlace_batch_create.cpp
  src/interlace/interlace_batch_scan.cpp
  src/interlace/interlace_batch_validate.cpp
  src/interlace/interlace_identity.cpp
)

add_library(svp::builder ALIAS svp-builder-lib)

target_include_directories(svp-builder-lib
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/src/interlace
)

target_compile_features(svp-builder-lib PUBLIC cxx_std_20)

target_link_libraries(svp-builder-lib
  PUBLIC
    svp::vision
    svp::progress
  PRIVATE
    nlohmann_json::nlohmann_json
    svp::audio
    svp::core
    svp::exec
    svp::media
    svp::models
    svp::package
    svp::validation
    libzip::zip
)

add_dependencies(svp-builder-lib svp-runtime-resources)

add_executable(svp-builder
  src/main.cpp
  src/cli/cli_register.cpp
  src/cli/cli_elapsed.cpp
  src/cli/cli_progress.cpp
  src/cli/cli_run_telemetry.cpp
  src/cli/cli_completion.cpp
  src/cli/probe_command.cpp
  src/cli/build_command.cpp
  src/cli/build_selected_output.cpp
  src/cli/diarize_command.cpp
  src/cli/diarize_replay_command.cpp
  src/cli/interlace_commands.cpp
  src/cli/transport_commands.cpp
  src/cli/interlace_output.cpp
  src/cli/main_dispatch.cpp
  src/cli/cli_runtime_tools.cpp
)

target_compile_features(svp-builder PRIVATE cxx_std_20)

target_include_directories(svp-builder PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(svp-builder
  PRIVATE
    CLI11::CLI11
    svp::builder
    svp::audio
    svp::core
    svp::media
    svp::package
    svp::validation
)

if(BUILD_TESTING)
  set(SVP_BUILDER_RUNTIME_RESOURCE_TEST_CWD
      "${CMAKE_CURRENT_BINARY_DIR}/runtime-resource-cwd")
  file(MAKE_DIRECTORY "${SVP_BUILDER_RUNTIME_RESOURCE_TEST_CWD}")

  add_executable(svp-builder-reference-model-ids-tests
    tests/reference_processor_model_ids_tests.cpp
  )

  add_executable(svp-builder-pipeline-tests
    tests/build_pipeline_tests.cpp
  )

  add_executable(svp-builder-frame-plan-tests
    tests/build_frame_plan_tests.cpp
  )
  target_include_directories(svp-builder-frame-plan-tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
  )
  target_compile_features(svp-builder-frame-plan-tests PRIVATE cxx_std_20)
  target_link_libraries(svp-builder-frame-plan-tests
    PRIVATE
      svp::builder
      svp::vision
  )

  add_executable(svp-builder-model-cache-preflight-tests
    tests/model_cache_preflight_tests.cpp
  )

  add_executable(svp-builder-progress-renderer-tests
    tests/progress_renderer_tests.cpp
  )

  add_executable(svp-builder-output-path-tests
    tests/output_path_tests.cpp
  )

  add_executable(svp-builder-progress-timeline-tests
    tests/progress_timeline_tests.cpp
  )

  add_executable(svp-builder-run-report-tests
    tests/build_run_report_tests.cpp
  )

  add_executable(svp-builder-thread-plan-tests
    tests/build_thread_plan_tests.cpp
  )

  add_executable(svp-builder-cli-elapsed-tests
    tests/cli_elapsed_tests.cpp
    src/cli/cli_completion.cpp
    src/cli/cli_elapsed.cpp
  )

  add_executable(svp-transport-output-policy-tests
    tests/transport_output_policy_tests.cpp
  )

  add_executable(svp-builder-runtime-tools-tests
    tests/runtime_tools_tests.cpp
  )
  target_include_directories(svp-builder-runtime-tools-tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
  )
  target_compile_features(svp-builder-runtime-tools-tests PRIVATE cxx_std_20)
  target_link_libraries(svp-builder-runtime-tools-tests
    PRIVATE
      svp::builder
      svp::exec
      nlohmann_json::nlohmann_json
  )

  add_executable(svp-interlace-tests
    tests/interlace_tests.cpp
  )

  add_executable(svp-interlace-batch-tests
    tests/interlace_batch_tests.cpp
  )

  target_include_directories(svp-builder-pipeline-tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
  )
  target_compile_features(svp-builder-pipeline-tests PRIVATE cxx_std_20)
  target_compile_options(svp-builder-pipeline-tests PRIVATE -UNDEBUG)
  target_compile_features(svp-builder-model-cache-preflight-tests PRIVATE cxx_std_20)
  target_compile_options(svp-builder-model-cache-preflight-tests PRIVATE -UNDEBUG)
  target_compile_features(svp-builder-progress-renderer-tests PRIVATE cxx_std_20)
  target_compile_options(svp-builder-progress-renderer-tests PRIVATE -UNDEBUG)
  target_compile_features(svp-builder-progress-timeline-tests PRIVATE cxx_std_20)
  target_compile_options(svp-builder-progress-timeline-tests PRIVATE -UNDEBUG)
  target_compile_features(svp-builder-run-report-tests PRIVATE cxx_std_20)
  target_compile_options(svp-builder-run-report-tests PRIVATE -UNDEBUG)
  target_compile_features(svp-builder-thread-plan-tests PRIVATE cxx_std_20)
  target_compile_options(svp-builder-thread-plan-tests PRIVATE -UNDEBUG)
  target_compile_features(svp-builder-cli-elapsed-tests PRIVATE cxx_std_20)
  target_compile_options(svp-builder-cli-elapsed-tests PRIVATE -UNDEBUG)
  target_compile_features(svp-transport-output-policy-tests PRIVATE cxx_std_20)
  target_compile_options(svp-transport-output-policy-tests PRIVATE -UNDEBUG)
  target_compile_features(svp-interlace-tests PRIVATE cxx_std_20)
  target_compile_features(svp-interlace-batch-tests PRIVATE cxx_std_20)

  target_link_libraries(svp-builder-reference-model-ids-tests
    PRIVATE
      svp::audio
      svp::models
      svp::vision
  )

  target_link_libraries(svp-builder-pipeline-tests
    PRIVATE
      svp::builder
      svp::models
  )

  target_link_libraries(svp-builder-model-cache-preflight-tests
    PRIVATE
      svp::builder
      svp::models
      svp::package
      svp::validation
  )

  target_link_libraries(svp-builder-progress-renderer-tests
    PRIVATE
      svp::builder
  )

  target_compile_features(svp-builder-output-path-tests PRIVATE cxx_std_20)
  target_compile_options(svp-builder-output-path-tests PRIVATE -UNDEBUG)
  target_link_libraries(svp-builder-output-path-tests
    PRIVATE
      svp::builder
      svp::models
      svp::package
      svp::validation
  )

  target_link_libraries(svp-builder-progress-timeline-tests
    PRIVATE
      svp::builder
      nlohmann_json::nlohmann_json
  )

  target_link_libraries(svp-builder-run-report-tests
    PRIVATE
      svp::builder
      nlohmann_json::nlohmann_json
  )

  target_link_libraries(svp-builder-thread-plan-tests
    PRIVATE
      svp::builder
      nlohmann_json::nlohmann_json
  )

  target_include_directories(svp-builder-cli-elapsed-tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
  )

  target_link_libraries(svp-interlace-tests
    PRIVATE
      svp::builder
      svp::models
      svp::package
      svp::validation
      libzip::zip
  )

  target_include_directories(svp-transport-output-policy-tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
  )
  target_link_libraries(svp-transport-output-policy-tests PRIVATE
    svp::builder
    svp::package
  )

  target_link_libraries(svp-interlace-batch-tests
    PRIVATE
      svp::builder
      svp::package
      svp::validation
      svp::query
      libzip::zip
  )

  add_test(NAME svp-builder-reference-model-ids-tests
           COMMAND svp-builder-reference-model-ids-tests
                   ${CMAKE_SOURCE_DIR}/spec/registries/reference-model-set.json)
  add_test(NAME svp-builder-frame-plan-tests
           COMMAND svp-builder-frame-plan-tests)
  add_test(NAME svp-builder-pipeline-tests COMMAND svp-builder-pipeline-tests
           WORKING_DIRECTORY ${SVP_BUILDER_RUNTIME_RESOURCE_TEST_CWD})
  add_test(NAME svp-builder-model-cache-preflight-tests
           COMMAND svp-builder-model-cache-preflight-tests)
  add_test(NAME svp-builder-help-without-model-cache COMMAND svp-builder --help)
  add_test(NAME svp-builder-version-without-model-cache COMMAND svp-builder --version)
  add_test(NAME svp-builder-probe-help-without-model-cache
           COMMAND svp-builder probe --help)
  set_tests_properties(
    svp-builder-help-without-model-cache
    svp-builder-version-without-model-cache
    svp-builder-probe-help-without-model-cache
    PROPERTIES
      ENVIRONMENT
        "SVP_MODEL_CACHE_DIR=${CMAKE_CURRENT_BINARY_DIR}/missing-model-cache"
  )
  add_test(NAME svp-builder-progress-renderer-tests COMMAND svp-builder-progress-renderer-tests)
  add_test(NAME svp-builder-output-path-tests
           COMMAND svp-builder-output-path-tests $<TARGET_FILE:svp-builder>
           WORKING_DIRECTORY ${SVP_BUILDER_RUNTIME_RESOURCE_TEST_CWD})
  add_test(NAME svp-builder-progress-timeline-tests
           COMMAND svp-builder-progress-timeline-tests)
  add_test(NAME svp-builder-run-report-tests COMMAND svp-builder-run-report-tests)
  add_test(NAME svp-builder-thread-plan-tests COMMAND svp-builder-thread-plan-tests
           WORKING_DIRECTORY ${SVP_BUILDER_RUNTIME_RESOURCE_TEST_CWD})
  add_test(NAME svp-builder-runtime-tools-tests COMMAND svp-builder-runtime-tools-tests)
  add_test(NAME svp-builder-cli-elapsed-tests COMMAND svp-builder-cli-elapsed-tests)
  add_test(NAME svp-transport-output-policy-tests
           COMMAND svp-transport-output-policy-tests)
  add_test(NAME svp-interlace-tests COMMAND svp-interlace-tests
           WORKING_DIRECTORY ${SVP_BUILDER_RUNTIME_RESOURCE_TEST_CWD})
  add_test(NAME svp-interlace-batch-tests COMMAND svp-interlace-batch-tests
           WORKING_DIRECTORY ${SVP_BUILDER_RUNTIME_RESOURCE_TEST_CWD})
endif()
