find_package(libzip CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(SQLite3 REQUIRED)
find_path(BLAKE3_INCLUDE_DIR NAMES blake3.h REQUIRED)
find_library(BLAKE3_LIBRARY NAMES blake3 REQUIRED)

add_library(svp-validation
  src/block_stream_validation.cpp
  src/color_record_validation.cpp
  src/code_registry.cpp
  src/diarization_validation.cpp
  src/entity_record_validation.cpp
  src/embedded_svpi_transport_validator.cpp
  src/embedded_media_binding_validation.cpp
  src/index_validation.cpp
  src/json_schema_subset.cpp
  src/ocr_color_spec.cpp
  src/ocr_color_index_consistency.cpp
  src/record_file_reader.cpp
  src/report.cpp
  src/report_json.cpp
  src/runtime_resources.cpp
  src/spec_assets.cpp
  src/text_record_validation.cpp
  src/validator.cpp
  src/svpi_validator.cpp
)

add_library(svp::validation ALIAS svp-validation)

target_include_directories(svp-validation
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
  PRIVATE
    ${BLAKE3_INCLUDE_DIR}
)

target_compile_features(svp-validation PUBLIC cxx_std_20)

target_link_libraries(svp-validation
  PUBLIC
    nlohmann_json::nlohmann_json
  PRIVATE
    libzip::zip
    SQLite::SQLite3
    ${BLAKE3_LIBRARY}
    svp::blocks
    svp::core
    svp::package
)

add_dependencies(svp-validation svp-runtime-resources)

if(BUILD_TESTING)
  set(SVP_VALIDATION_RUNTIME_RESOURCE_TEST_CWD
      "${CMAKE_CURRENT_BINARY_DIR}/runtime-resource-cwd")
  file(MAKE_DIRECTORY "${SVP_VALIDATION_RUNTIME_RESOURCE_TEST_CWD}")

  add_executable(svp-diarization-validation-tests
    tests/diarization_validation_tests.cpp
  )

  add_executable(svp-embedded-svpi-validation-tests
    tests/embedded_svpi_validation_tests.cpp
  )

  add_executable(svp-entity-record-validation-tests
    tests/entity_record_validation_tests.cpp
  )

  add_executable(svp-runtime-resource-tests
    tests/runtime_resource_tests.cpp
  )

  target_compile_features(svp-diarization-validation-tests PRIVATE cxx_std_20)
  target_compile_features(svp-embedded-svpi-validation-tests PRIVATE cxx_std_20)
  target_compile_features(svp-entity-record-validation-tests PRIVATE cxx_std_20)
  target_compile_features(svp-runtime-resource-tests PRIVATE cxx_std_20)
  target_compile_options(svp-runtime-resource-tests PRIVATE -UNDEBUG)
  target_compile_definitions(svp-runtime-resource-tests PRIVATE
    SVP_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
    SVP_RUNTIME_RESOURCE_DIR="${SVP_RUNTIME_RESOURCE_ROOT}"
    SVP_VALID_FIXTURE="${CMAKE_SOURCE_DIR}/fixtures/phase-04-ocr-color/valid/dashboard_number.svp"
  )
  set_target_properties(svp-runtime-resource-tests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tools/svp-validation"
  )
  set_target_properties(svp-embedded-svpi-validation-tests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tools/svp-validation"
  )

  target_link_libraries(svp-diarization-validation-tests
    PRIVATE
      svp::validation
      svp::package
  )

  target_link_libraries(svp-embedded-svpi-validation-tests PRIVATE
    svp::validation
    svp::package
    libzip::zip
  )

  target_link_libraries(svp-entity-record-validation-tests PRIVATE
    svp::validation
    svp::package
  )

  target_link_libraries(svp-runtime-resource-tests PRIVATE
    svp::core
    svp::validation
  )

  add_test(NAME svp-diarization-validation-tests
           COMMAND svp-diarization-validation-tests
           WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
  add_test(NAME svp-embedded-svpi-validation-tests
           COMMAND svp-embedded-svpi-validation-tests
           WORKING_DIRECTORY ${SVP_VALIDATION_RUNTIME_RESOURCE_TEST_CWD})
  add_test(NAME svp-entity-record-validation-tests
           COMMAND svp-entity-record-validation-tests
           WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
  add_test(NAME svp-runtime-resource-tests
           COMMAND svp-runtime-resource-tests)
endif()
