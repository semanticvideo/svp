find_package(nlohmann_json CONFIG REQUIRED)

add_library(svp-validation
  src/report.cpp
  src/report_json.cpp
)

add_library(svp::validation ALIAS svp-validation)

target_include_directories(svp-validation
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_compile_features(svp-validation PUBLIC cxx_std_20)

target_link_libraries(svp-validation
  PUBLIC
    nlohmann_json::nlohmann_json
)

