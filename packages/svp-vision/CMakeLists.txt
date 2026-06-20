find_package(nlohmann_json CONFIG REQUIRED)

add_library(svp-vision
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
  add_executable(svp-vision-tests
    tests/observation_pipeline_plan_tests.cpp
  )

  target_compile_features(svp-vision-tests PRIVATE cxx_std_20)

  target_link_libraries(svp-vision-tests
    PRIVATE
      svp::vision
  )

  add_test(NAME svp-vision-tests COMMAND svp-vision-tests)
endif()
