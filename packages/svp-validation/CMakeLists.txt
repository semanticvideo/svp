find_package(libzip CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(SQLite3 REQUIRED)
find_path(BLAKE3_INCLUDE_DIR NAMES blake3.h REQUIRED)
find_library(BLAKE3_LIBRARY NAMES blake3 REQUIRED)

add_library(svp-validation
  src/block_stream_validation.cpp
  src/color_record_validation.cpp
  src/code_registry.cpp
  src/index_validation.cpp
  src/json_schema_subset.cpp
  src/ocr_color_spec.cpp
  src/ocr_color_index_consistency.cpp
  src/record_file_reader.cpp
  src/report.cpp
  src/report_json.cpp
  src/spec_assets.cpp
  src/text_record_validation.cpp
  src/validator.cpp
)

add_library(svp::validation ALIAS svp-validation)

target_include_directories(svp-validation
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
  PRIVATE
    ${BLAKE3_INCLUDE_DIR}
)

target_compile_features(svp-validation PUBLIC cxx_std_20)

target_link_libraries(svp-validation
  PUBLIC
    nlohmann_json::nlohmann_json
  PRIVATE
    libzip::zip
    SQLite::SQLite3
    ${BLAKE3_LIBRARY}
    svp::blocks
    svp::package
)
