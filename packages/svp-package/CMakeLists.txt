find_package(libzip CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(SQLite3 REQUIRED)
find_path(BLAKE3_INCLUDE_DIR NAMES blake3.h REQUIRED)
find_library(BLAKE3_LIBRARY NAMES blake3 REQUIRED)

add_library(svp-package
  src/package_contract.cpp
  src/package_layout.cpp
  src/package_probe.cpp
  src/package_summary.cpp
  src/package_writer.cpp
  src/index_logical_rows.cpp
  src/index_writer.cpp
  src/relationship_provenance_writer.cpp
  src/spatial_embedding_placeholders.cpp
  src/timeline_writer.cpp
  src/validation_report_storage.cpp
)

add_library(svp::package ALIAS svp-package)

target_include_directories(svp-package
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
  PRIVATE
    ${BLAKE3_INCLUDE_DIR}
)

target_compile_features(svp-package PUBLIC cxx_std_20)

target_link_libraries(svp-package
  PUBLIC
    nlohmann_json::nlohmann_json
    svp::core
  PRIVATE
    libzip::zip
    SQLite::SQLite3
    ${BLAKE3_LIBRARY}
    svp::vision
    svp::media
)

if(BUILD_TESTING)
  add_executable(svp-package-tests
    tests/package_writer_tests.cpp
  )

  target_compile_features(svp-package-tests PRIVATE cxx_std_20)

  target_link_libraries(svp-package-tests
    PRIVATE
      svp::package
      svp::media
      svp::vision
  )

  add_test(NAME svp-package-tests COMMAND svp-package-tests)
endif()
