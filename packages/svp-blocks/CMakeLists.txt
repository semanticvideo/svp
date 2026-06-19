add_library(svp-blocks
  src/block_stream.cpp
)

add_library(svp::blocks ALIAS svp-blocks)

target_include_directories(svp-blocks
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_compile_features(svp-blocks PUBLIC cxx_std_20)
