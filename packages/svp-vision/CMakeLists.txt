find_package(nlohmann_json CONFIG REQUIRED)

add_library(svp-vision
  src/color_quantization.cpp
  src/observation_pipeline_plan.cpp
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
    svp::media
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
endif()
