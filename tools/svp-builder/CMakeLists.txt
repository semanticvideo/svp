find_package(CLI11 CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)

add_library(svp-builder-lib
  src/audio_stage.cpp
  src/build_pipeline.cpp
  src/build_pipeline_io.cpp
  src/build_progress.cpp
  src/build_stage_metadata.cpp
  src/foundation_color_stage.cpp
  src/foundation_ocr_stage.cpp
  src/package_skeleton_stage.cpp
  src/vision_plan_stage.cpp
)

add_library(svp::builder ALIAS svp-builder-lib)

target_include_directories(svp-builder-lib
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_compile_features(svp-builder-lib PUBLIC cxx_std_20)

target_link_libraries(svp-builder-lib
  PRIVATE
    nlohmann_json::nlohmann_json
    svp::audio
    svp::core
    svp::media
    svp::models
    svp::vision
    svp::package
    svp::validation
)

add_executable(svp-builder
  src/main.cpp
)

target_compile_features(svp-builder PRIVATE cxx_std_20)

target_link_libraries(svp-builder
  PRIVATE
    CLI11::CLI11
    svp::builder
    svp::core
    svp::media
)

if(BUILD_TESTING)
  add_executable(svp-builder-pipeline-tests
    tests/build_pipeline_tests.cpp
  )

  target_compile_features(svp-builder-pipeline-tests PRIVATE cxx_std_20)

  target_link_libraries(svp-builder-pipeline-tests
    PRIVATE
      svp::builder
  )

  add_test(NAME svp-builder-pipeline-tests COMMAND svp-builder-pipeline-tests)
endif()
