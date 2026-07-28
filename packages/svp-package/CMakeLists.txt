find_package(libzip CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(SQLite3 REQUIRED)
find_path(BLAKE3_INCLUDE_DIR NAMES blake3.h REQUIRED)
find_library(BLAKE3_LIBRARY NAMES blake3 REQUIRED)
find_path(ZSTD_INCLUDE_DIR NAMES zstd.h REQUIRED)
find_library(ZSTD_LIBRARY NAMES zstd REQUIRED)

add_library(svp-package
  src/package_contract.cpp
  src/package_layout.cpp
  src/package_source.cpp
  src/embedded_package_session.cpp
  src/package_probe.cpp
  src/package_summary.cpp
  src/isobmff_top_level.cpp
  src/iso_bmff_container.cpp
  src/embedded_svpi_box.cpp
  src/embedded_file_io.cpp
  src/embedded_isobmff_copy.cpp
  src/embedded_svpi_reader.cpp
  src/embedded_svpi_writer.cpp
  src/embedded_svpi_extract.cpp
  src/embedded_svpi_strip.cpp
  src/package_writer.cpp
  src/index_logical_rows.cpp
  src/index_writer.cpp
  src/entity_writer.cpp
  src/relationship_provenance_writer.cpp
  src/relationship_type_policy.cpp
  src/spatial_embedding_placeholders.cpp
  src/timeline_writer.cpp
  src/validation_report_storage.cpp
  src/visual_entity_artifact_writer.cpp
  src/media_binding.cpp
  src/media_binding_factory.cpp
  src/svpi_writer.cpp
  src/svpi_media_policy.cpp
)

add_library(svp::package ALIAS svp-package)

add_executable(svp-visual-entity-streaming-diagnostic
  tools/visual_entity_streaming_diagnostic.cpp
)

target_compile_features(svp-visual-entity-streaming-diagnostic PRIVATE cxx_std_20)

target_link_libraries(svp-visual-entity-streaming-diagnostic PRIVATE
  svp::package
  svp::media
  svp::vision
)

target_include_directories(svp-package
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
  PRIVATE
    ${BLAKE3_INCLUDE_DIR}
    ${ZSTD_INCLUDE_DIR}
)

target_compile_features(svp-package PUBLIC cxx_std_20)

target_link_libraries(svp-package
  PUBLIC
    nlohmann_json::nlohmann_json
    svp::core
    svp::vision
  PRIVATE
    libzip::zip
    SQLite::SQLite3
    ${BLAKE3_LIBRARY}
    ${ZSTD_LIBRARY}
    svp::media
)

if(BUILD_TESTING)
  add_executable(svp-package-tests
    tests/package_writer_tests.cpp
  )

  add_executable(svp-svpi-tests
    tests/svpi_writer_tests.cpp
  )

  add_executable(svp-relationship-type-policy-tests
    tests/relationship_type_policy_tests.cpp
  )

  add_executable(svp-visual-entity-artifact-writer-tests
    tests/visual_entity_artifact_writer_tests.cpp
  )

  add_executable(svp-embedded-svpi-tests
    tests/embedded_svpi_tests_main.cpp
    tests/embedded_svpi_test_support.cpp
    tests/embedded_svpi_reader_tests.cpp
    tests/embedded_svpi_writer_tests.cpp
  )

  target_compile_features(svp-package-tests PRIVATE cxx_std_20)
  target_compile_features(svp-svpi-tests PRIVATE cxx_std_20)
  target_compile_features(svp-relationship-type-policy-tests PRIVATE cxx_std_20)
  target_compile_features(svp-visual-entity-artifact-writer-tests PRIVATE cxx_std_20)
  target_compile_features(svp-embedded-svpi-tests PRIVATE cxx_std_20)
  target_include_directories(svp-embedded-svpi-tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
  )

  target_compile_definitions(svp-svpi-tests PRIVATE
    SVP_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
  )

  target_link_libraries(svp-package-tests
    PRIVATE
      svp::package
      svp::media
      svp::vision
  )

  target_link_libraries(svp-svpi-tests
    PRIVATE
      svp::package
      svp::validation
      SQLite::SQLite3
  )

  target_link_libraries(svp-relationship-type-policy-tests
    PRIVATE
      svp::package
  )

  target_link_libraries(svp-visual-entity-artifact-writer-tests PRIVATE
    svp::package
  )

  target_link_libraries(svp-embedded-svpi-tests PRIVATE svp::package)

  add_test(NAME svp-package-tests COMMAND svp-package-tests)
  add_test(NAME svp-svpi-tests COMMAND svp-svpi-tests)
  add_test(NAME svp-relationship-type-policy-tests COMMAND svp-relationship-type-policy-tests)
  add_test(NAME svp-visual-entity-artifact-writer-tests COMMAND svp-visual-entity-artifact-writer-tests)
  add_test(NAME svp-embedded-svpi-tests COMMAND svp-embedded-svpi-tests)
endif()
