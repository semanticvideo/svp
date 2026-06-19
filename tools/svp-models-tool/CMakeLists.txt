find_package(CLI11 CONFIG REQUIRED)

add_executable(svp-models-tool
  src/main.cpp
)

target_compile_features(svp-models-tool PRIVATE cxx_std_20)

target_link_libraries(svp-models-tool
  PRIVATE
    CLI11::CLI11
    svp::models
)
