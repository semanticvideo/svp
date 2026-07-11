find_package(nlohmann_json CONFIG REQUIRED)

find_path(BLAKE3_INCLUDE_DIR NAMES blake3.h REQUIRED)
find_library(BLAKE3_LIBRARY NAMES blake3 REQUIRED)

# ONNX Runtime is optional. When found, SVP_MODEL_ONNX_RUNTIME_AVAILABLE is defined
# and the runtime layer links against onnxruntime. When not found, the runtime
# layer gracefully reports ONNX Runtime as unavailable.
# ONNX Runtime's vcpkg config requests lowercase `protobuf`, while CMake's
# module publishes `Protobuf_FOUND`. Bridge that package-name mismatch so the
# config continues loading its exported targets on every supported platform.
find_package(Protobuf QUIET)
if(Protobuf_FOUND)
  set(protobuf_FOUND TRUE)
endif()
find_package(utf8_range CONFIG QUIET)
find_package(onnxruntime CONFIG QUIET)
if(TARGET onnxruntime::onnxruntime)
  set(SVP_MODEL_ONNX_RUNTIME_AVAILABLE TRUE)
  # The Debug ONNX static archive trips its schema-registry assertion when
  # loaded through ONNX Runtime; keep Debug app builds on the Release archives.
  foreach(SVP_ONNX_TARGET
      onnxruntime::onnxruntime_flatbuffers
      onnxruntime::onnxruntime_common
      onnxruntime::onnxruntime_mlas
      onnxruntime::onnxruntime_graph
      onnxruntime::onnxruntime_lora
      onnxruntime::onnxruntime_framework
      onnxruntime::onnxruntime_util
      onnxruntime::onnxruntime_providers
      onnxruntime::onnxruntime_optimizer
      onnxruntime::onnxruntime_session
      ONNX::onnx
      ONNX::onnx_proto)
    if(TARGET ${SVP_ONNX_TARGET})
      set_property(TARGET ${SVP_ONNX_TARGET}
                   PROPERTY MAP_IMPORTED_CONFIG_DEBUG Release)
    endif()
  endforeach()
else()
  set(SVP_MODEL_ONNX_RUNTIME_AVAILABLE FALSE)
endif()

if(SVP_MODEL_ONNX_RUNTIME_AVAILABLE)
  find_path(SVP_ONNXRUNTIME_INCLUDE_DIR
    NAMES onnxruntime_cxx_api.h
    PATH_SUFFIXES onnxruntime onnxruntime/core/session
    REQUIRED)

  find_path(SVP_COREML_PROVIDER_INCLUDE_DIR
    NAMES coreml_provider_factory.h
    HINTS ${SVP_ONNXRUNTIME_INCLUDE_DIR}
    PATH_SUFFIXES . onnxruntime core/providers/coreml onnxruntime/core/providers/coreml)

  find_library(SVP_ONNXRUNTIME_COREML_PROVIDER_LIBRARY
    NAMES onnxruntime_providers_coreml)
  find_library(SVP_COREML_PROTO_LIBRARY
    NAMES coreml_proto)
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
  target_compile_definitions(svp-models PUBLIC SVP_ONNX_RUNTIME_AVAILABLE=1)
  target_include_directories(svp-models PUBLIC ${SVP_ONNXRUNTIME_INCLUDE_DIR})
  if(SVP_COREML_PROVIDER_INCLUDE_DIR)
    target_compile_definitions(svp-models PRIVATE SVP_COREML_AVAILABLE=1)
    target_include_directories(svp-models PRIVATE ${SVP_COREML_PROVIDER_INCLUDE_DIR})
    if(SVP_ONNXRUNTIME_COREML_PROVIDER_LIBRARY)
      target_link_libraries(svp-models PRIVATE ${SVP_ONNXRUNTIME_COREML_PROVIDER_LIBRARY})
    endif()
    if(SVP_COREML_PROTO_LIBRARY)
      target_link_libraries(svp-models PRIVATE ${SVP_COREML_PROTO_LIBRARY})
    endif()
  endif()

  # Keep ONNX headers and runtime library from the same CMake package. Sherpa's
  # C API is discovered separately by the audio diarization boundary.
  target_link_libraries(svp-models PUBLIC onnxruntime::onnxruntime)
endif()

target_link_libraries(svp-models
  PUBLIC
    nlohmann_json::nlohmann_json
    svp::core
  PRIVATE
    ${BLAKE3_LIBRARY}
)
