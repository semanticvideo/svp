find_package(CLI11 CONFIG REQUIRED)

add_executable(svp-validator
  src/equivalence_output.cpp
  src/main.cpp
)

target_compile_features(svp-validator PRIVATE cxx_std_20)

target_link_libraries(svp-validator
  PRIVATE
    CLI11::CLI11
    svp::core
    svp::package
    svp::validation
)

add_dependencies(svp-validator svp-runtime-resources)

if(BUILD_TESTING)
  add_executable(svp-validator-runtime-resource-cwd-tests
    tests/runtime_resource_cwd_tests.cpp
  )
  target_compile_features(svp-validator-runtime-resource-cwd-tests PRIVATE
    cxx_std_20
  )
  add_dependencies(svp-validator-runtime-resource-cwd-tests
    svp-validator
    svp-runtime-resources
  )
  add_test(NAME svp-validator-runtime-resource-cwd-tests
    COMMAND svp-validator-runtime-resource-cwd-tests
            $<TARGET_FILE:svp-validator>
            ${CMAKE_SOURCE_DIR}/fixtures/phase-04-ocr-color/valid/dashboard_number.svp
            ${SVP_RUNTIME_RESOURCE_ROOT}/registries/validation-codes.json
  )

  set(SVP_EQUIVALENCE_FIXTURE_DIR ${CMAKE_SOURCE_DIR}/fixtures/phase-04-ocr-color)
  add_test(NAME svp-validator-equivalent-byte-identical
    COMMAND svp-validator validate --json --equivalent
            ${SVP_EQUIVALENCE_FIXTURE_DIR}/valid/dashboard_number.svp
            ${SVP_EQUIVALENCE_FIXTURE_DIR}/valid/dashboard_number.svp
  )
  set_tests_properties(svp-validator-equivalent-byte-identical PROPERTIES
    PASS_REGULAR_EXPRESSION "\"classification\": \"byte_identical\""
  )
  add_test(NAME svp-validator-equivalent-not-equivalent
    COMMAND svp-validator validate --equivalent
            ${SVP_EQUIVALENCE_FIXTURE_DIR}/valid/dashboard_number.svp
            ${SVP_EQUIVALENCE_FIXTURE_DIR}/invalid/invalid_color_totals.svp
  )
  set_tests_properties(svp-validator-equivalent-not-equivalent PROPERTIES
    WILL_FAIL TRUE
  )
endif()
