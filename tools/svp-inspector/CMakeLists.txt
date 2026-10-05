find_package(CLI11 CONFIG REQUIRED)
find_package(libzip CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)

# `svp-inspector export` (docs/svpi/Package_Export_v1.md), kept in its own
# library so its tests can drive the pieces directly.
add_library(svp-inspector-export STATIC
  src/export/block_stream_export.cpp
  src/export/entry_path_policy.cpp
  src/export/export_command.cpp
  src/export/export_error.cpp
  src/export/export_plan.cpp
  src/export/export_summary.cpp
  src/export/input_identity.cpp
  src/export/jsonl_line_reader.cpp
  src/export/jsonl_record.cpp
  src/export/layer_writers.cpp
  src/export/output_transaction.cpp
  src/export/package_kind.cpp
  src/export/package_reader.cpp
  src/export/reference_resolver.cpp
  src/export/reference_rules.cpp
  src/export/validation_gate.cpp
  src/export/version_policy.cpp
)

target_compile_features(svp-inspector-export PUBLIC cxx_std_20)

target_include_directories(svp-inspector-export
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(svp-inspector-export
  PUBLIC
    nlohmann_json::nlohmann_json
    svp::blocks
    svp::core
    svp::package
    svp::validation
  PRIVATE
    libzip::zip
)

add_executable(svp-inspector
  src/main.cpp
  src/dump_command.cpp
  src/embedded_input_controller.cpp
  src/embedded_transport_output.cpp
  src/inspect_output.cpp
  src/query_output.cpp
)

target_compile_features(svp-inspector PRIVATE cxx_std_20)

target_link_libraries(svp-inspector
  PRIVATE
    CLI11::CLI11
    svp::core
    svp::package
    svp::query
    svp-inspector-export
)

# export validates packages with the default validator resources.
add_dependencies(svp-inspector svp-runtime-resources)

if(BUILD_TESTING)
  add_executable(svp-inspector-embedded-input-tests
    tests/embedded_input_controller_tests.cpp
    src/embedded_input_controller.cpp
  )
  target_compile_features(svp-inspector-embedded-input-tests PRIVATE cxx_std_20)
  target_include_directories(svp-inspector-embedded-input-tests PRIVATE src)
  target_link_libraries(svp-inspector-embedded-input-tests PRIVATE svp::package)
  add_test(NAME svp-inspector-embedded-input-tests
           COMMAND svp-inspector-embedded-input-tests)

  add_subdirectory(tests/export)
endif()
