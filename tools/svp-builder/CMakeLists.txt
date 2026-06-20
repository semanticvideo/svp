find_package(CLI11 CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)

add_executable(svp-builder
  src/main.cpp
)

target_compile_features(svp-builder PRIVATE cxx_std_20)

target_link_libraries(svp-builder
  PRIVATE
    CLI11::CLI11
    nlohmann_json::nlohmann_json
    svp::audio
    svp::core
    svp::media
    svp::models
    svp::vision
)
