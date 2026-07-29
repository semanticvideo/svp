find_package(CLI11 CONFIG REQUIRED)

add_executable(svp-validator
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
            ${CMAKE_SOURCE_DIR}/spec/registries/validation-codes.json
  )
endif()
