find_package(nlohmann_json CONFIG REQUIRED)

add_library(svp-vision
  src/color_frame_sampling.cpp
  src/color_observation_records.cpp
  src/color_quantization.cpp
  src/depth_generation.cpp
  src/embedding_generation.cpp
  src/foundation_color_observations.cpp
  src/foundation_color_staging.cpp
  src/foundation_ocr_staging.cpp
  src/observation_pipeline_plan.cpp
  src/real_frame_color_sampling.cpp
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
)

if(BUILD_TESTING)
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
endif()
