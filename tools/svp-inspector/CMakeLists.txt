find_package(CLI11 CONFIG REQUIRED)

add_executable(svp-inspector
  src/main.cpp
  src/embedded_inspection_output.cpp
)

target_compile_features(svp-inspector PRIVATE cxx_std_20)

target_link_libraries(svp-inspector
  PRIVATE
    CLI11::CLI11
    svp::core
    svp::package
    svp::query
)
