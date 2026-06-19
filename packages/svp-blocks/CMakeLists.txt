find_path(BLAKE3_INCLUDE_DIR NAMES blake3.h REQUIRED)
find_library(BLAKE3_LIBRARY NAMES blake3 REQUIRED)
find_path(ZSTD_INCLUDE_DIR NAMES zstd.h REQUIRED)
find_library(ZSTD_LIBRARY NAMES zstd REQUIRED)

add_library(svp-blocks
  src/block_stream.cpp
)

add_library(svp::blocks ALIAS svp-blocks)

target_include_directories(svp-blocks
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
  PRIVATE
    ${BLAKE3_INCLUDE_DIR}
    ${ZSTD_INCLUDE_DIR}
)

target_compile_features(svp-blocks PUBLIC cxx_std_20)

target_link_libraries(svp-blocks
  PRIVATE
    ${BLAKE3_LIBRARY}
    ${ZSTD_LIBRARY}
)
