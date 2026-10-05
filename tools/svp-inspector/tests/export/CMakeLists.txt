add_library(svp-inspector-export-test-support STATIC
  export_test_support.cpp
  svpi_fixture_builder.cpp
)
target_compile_features(svp-inspector-export-test-support PUBLIC cxx_std_20)
target_include_directories(svp-inspector-export-test-support
  PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}
)
target_link_libraries(svp-inspector-export-test-support
  PUBLIC
    nlohmann_json::nlohmann_json
  PRIVATE
    libzip::zip
    svp::blocks
    svp::package
)

add_executable(svp-inspector-export-unit-tests export_unit_tests.cpp)
target_link_libraries(svp-inspector-export-unit-tests PRIVATE
  svp-inspector-export
  svp-inspector-export-test-support
)
add_test(NAME svp-inspector-export-unit-tests
         COMMAND svp-inspector-export-unit-tests)

add_executable(svp-inspector-export-cli-tests export_cli_tests.cpp)
target_link_libraries(svp-inspector-export-cli-tests PRIVATE
  svp-inspector-export-test-support
  svp::package
)
add_dependencies(svp-inspector-export-cli-tests svp-inspector)
target_compile_definitions(svp-inspector-export-cli-tests PRIVATE
  SVP_EXPORT_SCHEMA_DIR="${CMAKE_SOURCE_DIR}/spec/schemas"
)

set(SVP_EXPORT_TEST_FIXTURE_DIR "${CMAKE_SOURCE_DIR}/fixtures/phase-04-ocr-color")
foreach(export_case IN ITEMS
    svp-detection
    svpi-layers
    embedded-input
    determinism
    invalid-package
    unsupported-version
    path-traversal
    output-rules
    malformed-content
    unreadable-input
    schema-conformance)
  add_test(NAME svp-inspector-export-${export_case}
    COMMAND svp-inspector-export-cli-tests
            $<TARGET_FILE:svp-inspector>
            ${SVP_EXPORT_TEST_FIXTURE_DIR}
            ${export_case}
  )
endforeach()

# Real packages are never committed; set SVP_EXPORT_REAL_PACKAGES_DIR to a
# directory of .svp/.svpi packages and embedded-SVPI .mp4/.mov/.m4v files to
# run this test, otherwise it is skipped.
add_executable(svp-inspector-export-real-package-tests
  export_real_package_tests.cpp
)
target_link_libraries(svp-inspector-export-real-package-tests PRIVATE
  svp-inspector-export-test-support
  svp::package
)
add_dependencies(svp-inspector-export-real-package-tests svp-inspector)
add_test(NAME svp-inspector-export-real-packages
  COMMAND svp-inspector-export-real-package-tests $<TARGET_FILE:svp-inspector>
)
set_tests_properties(svp-inspector-export-real-packages PROPERTIES
  SKIP_RETURN_CODE 77
)
