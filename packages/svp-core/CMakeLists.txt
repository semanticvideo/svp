add_library(svp-core
  src/hash_string.cpp
  src/id.cpp
  src/memory_diagnostics.cpp
  src/path.cpp
  src/process_stdio.cpp
  src/timestamp.cpp
  src/version.cpp
)

add_library(svp::core ALIAS svp-core)

target_include_directories(svp-core
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_compile_features(svp-core PUBLIC cxx_std_20)
