find_package(nlohmann_json CONFIG REQUIRED)

find_path(BLAKE3_INCLUDE_DIR NAMES blake3.h REQUIRED)
find_library(BLAKE3_LIBRARY NAMES blake3 REQUIRED)

# ONNX Runtime is optional. When found, SVP_MODEL_ONNX_RUNTIME_AVAILABLE is defined
# and the runtime layer links against onnxruntime. When not found, the runtime
# layer gracefully reports ONNX Runtime as unavailable.
find_package(onnxruntime CONFIG QUIET)
if(onnxruntime_FOUND)
  set(SVP_MODEL_ONNX_RUNTIME_AVAILABLE TRUE)
else()
  set(SVP_MODEL_ONNX_RUNTIME_AVAILABLE FALSE)
endif()

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

if(SVP_MODEL_ONNX_RUNTIME_AVAILABLE)
  target_compile_definitions(svp-models PRIVATE SVP_ONNX_RUNTIME_AVAILABLE=1)
  target_link_libraries(svp-models PRIVATE onnxruntime::onnxruntime)
endif()

target_link_libraries(svp-models
  PUBLIC
    nlohmann_json::nlohmann_json
    svp::core
  PRIVATE
    ${BLAKE3_LIBRARY}
)
