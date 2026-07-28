find_package(CLI11 CONFIG REQUIRED)

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
)

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
endif()
