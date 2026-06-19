find_package(nlohmann_json CONFIG REQUIRED)

find_path(BLAKE3_INCLUDE_DIR NAMES blake3.h REQUIRED)
find_library(BLAKE3_LIBRARY NAMES blake3 REQUIRED)

add_library(svp-models
  src/cache.cpp
  src/error.cpp
  src/hash.cpp
  src/manifest.cpp
  src/model_id.cpp
  src/model_lock.cpp
  src/reference_model_set.cpp
  src/runtime.cpp
  src/verification.cpp
)

add_library(svp::models ALIAS svp-models)

target_include_directories(svp-models
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
  PRIVATE
    ${BLAKE3_INCLUDE_DIR}
)

target_compile_features(svp-models PUBLIC cxx_std_20)

target_link_libraries(svp-models
  PUBLIC
    nlohmann_json::nlohmann_json
    svp::core
  PRIVATE
    ${BLAKE3_LIBRARY}
)
