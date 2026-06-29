find_package(nlohmann_json CONFIG REQUIRED)

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
)

if(BUILD_TESTING)
  add_executable(svp-query-tests
    tests/query_tests.cpp
  )

  target_compile_features(svp-query-tests PRIVATE cxx_std_20)

  target_link_libraries(svp-query-tests
    PRIVATE
      svp::query
  )

  add_test(NAME svp-query-tests COMMAND svp-query-tests)
endif()
