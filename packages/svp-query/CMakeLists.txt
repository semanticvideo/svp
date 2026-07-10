find_package(nlohmann_json CONFIG REQUIRED)
find_package(SQLite3 REQUIRED)

add_library(svp-query
  src/query_reader.cpp
  src/query_ops.cpp
  src/traversal.cpp
)

add_library(svp::query ALIAS svp-query)

target_include_directories(svp-query
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_compile_features(svp-query PUBLIC cxx_std_20)

target_link_libraries(svp-query
  PUBLIC
    nlohmann_json::nlohmann_json
    svp::package
  PRIVATE
    SQLite::SQLite3
)

if(BUILD_TESTING)
  add_executable(svp-query-tests
    tests/query_test_registry.cpp
    tests/query_test_io.cpp
    tests/fixtures/basic_package_fixture.cpp
    tests/fixtures/malformed_package_fixture.cpp
    tests/fixtures/relationship_package_fixture.cpp
    tests/fixtures/traversal_package_fixture.cpp
    tests/fixtures/semantic_package_fixture.cpp
    tests/fixtures/frame_connectivity_fixture.cpp
    tests/fixtures/spatial_pair_fixture.cpp
    tests/query_reader_tests.cpp
    tests/query_malformed_tests.cpp
    tests/relationship_query_tests.cpp
    tests/traversal_basic_tests.cpp
    tests/traversal_error_tests.cpp
    tests/traversal_semantic_tests.cpp
    tests/traversal_time_window_tests.cpp
    tests/path_tests.cpp
    tests/context_tests.cpp
    tests/graph_health_tests.cpp
    tests/frame_connectivity_tests.cpp
    tests/spatial_relationship_tests.cpp
    tests/traversal_backend_parity_tests.cpp
    tests/embedded_query_equivalence_tests.cpp
    tests/query_tests_main.cpp
  )

  target_compile_features(svp-query-tests PRIVATE cxx_std_20)

  target_link_libraries(svp-query-tests
    PRIVATE
      svp::query
      svp::package
      SQLite::SQLite3
  )

  add_test(NAME svp-query-tests COMMAND svp-query-tests)
endif()
