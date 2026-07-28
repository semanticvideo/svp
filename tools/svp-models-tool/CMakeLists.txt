find_package(CLI11 CONFIG REQUIRED)

add_executable(svp-models-tool
  src/main.cpp
)

target_compile_features(svp-models-tool PRIVATE cxx_std_20)

target_link_libraries(svp-models-tool
  PRIVATE
    CLI11::CLI11
    svp::models
)

if(BUILD_TESTING)
  add_test(NAME svp-models-tool-manifest-help
           COMMAND svp-models-tool verify --help)
  set_tests_properties(svp-models-tool-manifest-help PROPERTIES
    PASS_REGULAR_EXPRESSION "verifies its complete containing")

  add_test(NAME svp-models-tool-manifest-name
           COMMAND svp-models-tool verify --manifest not-a-manifest.json)
  set_tests_properties(svp-models-tool-manifest-name PROPERTIES
    WILL_FAIL TRUE)
endif()
