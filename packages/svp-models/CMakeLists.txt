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

if(SVP_MODEL_ONNX_RUNTIME_AVAILABLE)
  find_path(SVP_ONNXRUNTIME_INCLUDE_DIR
    NAMES onnxruntime_cxx_api.h
    PATH_SUFFIXES onnxruntime onnxruntime/core/session
    REQUIRED)

  find_path(SVP_COREML_PROVIDER_INCLUDE_DIR
    NAMES coreml_provider_factory.h
    PATH_SUFFIXES onnxruntime onnxruntime/core/providers/coreml)
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
  endif()

  # The vcpkg static onnxruntime (ONNX 1.19.0) has a schema double-registration
  # bug between libonnx.a and libonnxruntime_graph.a that blocks all ONNX model
  # loading. Prefer the dynamic onnxruntime shipped with sherpa-onnx, which does
  # not have this issue. Auto-discover it from common install locations.
  set(SVP_SHERPA_ORT_LIB "" CACHE PATH "Path to dynamic libonnxruntime from sherpa-onnx (auto-discovered if empty)")

  if(NOT SVP_SHERPA_ORT_LIB)
    # Auto-discover: scan common sherpa-onnx install locations for the dynamic
    # onnxruntime library. On macOS it's libonnxruntime.dylib, on Linux .so.
    set(_ort_names "libonnxruntime.dylib" "libonnxruntime.so")
    set(_ort_search_dirs "")

    # macOS user site-packages: ~/Library/Python/3.{9..14}/lib/python/site-packages/sherpa_onnx/lib
    if(DEFINED ENV{HOME})
      foreach(ver RANGE 9 14)
        list(APPEND _ort_search_dirs "$ENV{HOME}/Library/Python/3.${ver}/lib/python/site-packages/sherpa_onnx/lib")
      endforeach()
    endif()

    # Linux pip --user: ~/.local/lib/python3.*/site-packages/sherpa_onnx/lib
    if(DEFINED ENV{HOME})
      file(GLOB _linux_user_dirs "$ENV{HOME}/.local/lib/python3.*/site-packages/sherpa_onnx/lib")
      list(APPEND _ort_search_dirs ${_linux_user_dirs})
    endif()

    # venv and Conda
    if(DEFINED ENV{VIRTUAL_ENV})
      file(GLOB _venv_dirs "$ENV{VIRTUAL_ENV}/lib/python3.*/site-packages/sherpa_onnx/lib")
      list(APPEND _ort_search_dirs ${_venv_dirs})
      list(APPEND _ort_search_dirs "$ENV{VIRTUAL_ENV}/lib/sherpa_onnx/lib")
    endif()
    if(DEFINED ENV{CONDA_PREFIX})
      file(GLOB _conda_dirs "$ENV{CONDA_PREFIX}/lib/python3.*/site-packages/sherpa_onnx/lib")
      list(APPEND _ort_search_dirs ${_conda_dirs})
      list(APPEND _ort_search_dirs "$ENV{CONDA_PREFIX}/lib/sherpa_onnx/lib")
    endif()

    # Homebrew (Apple Silicon and Intel)
    file(GLOB _homebrew_dirs "/opt/homebrew/lib/python3.*/site-packages/sherpa_onnx/lib")
    list(APPEND _ort_search_dirs ${_homebrew_dirs})
    file(GLOB _homebrew_intel_dirs "/usr/local/lib/python3.*/site-packages/sherpa_onnx/lib")
    list(APPEND _ort_search_dirs ${_homebrew_intel_dirs})

    # Linux system paths
    file(GLOB _linux_sys_dirs "/usr/lib/python3.*/site-packages/sherpa_onnx/lib" "/usr/lib/python3.*/dist-packages/sherpa_onnx/lib")
    list(APPEND _ort_search_dirs ${_linux_sys_dirs})

    foreach(_dir IN LISTS _ort_search_dirs)
      foreach(_name IN LISTS _ort_names)
        if(EXISTS "${_dir}/${_name}")
          set(SVP_SHERPA_ORT_LIB "${_dir}/${_name}")
          break()
        endif()
      endforeach()
      if(SVP_SHERPA_ORT_LIB)
        break()
      endif()
    endforeach()
  endif()

  if(SVP_SHERPA_ORT_LIB AND EXISTS "${SVP_SHERPA_ORT_LIB}")
    message(STATUS "Using dynamic onnxruntime from sherpa-onnx: ${SVP_SHERPA_ORT_LIB}")
    target_link_libraries(svp-models PUBLIC ${SVP_SHERPA_ORT_LIB})
    target_compile_definitions(svp-models PUBLIC SVP_DYNAMIC_ORT=1)
  else()
    message(WARNING "sherpa-onnx dynamic onnxruntime not found. Falling back to "
      "static vcpkg onnxruntime, which has a known ONNX 1.19.0 schema "
      "double-registration bug that may block model loading. "
      "Install sherpa-onnx (pip3 install sherpa-onnx) to fix this.")
    target_link_libraries(svp-models PUBLIC onnxruntime::onnxruntime)
  endif()
endif()

target_link_libraries(svp-models
  PUBLIC
    nlohmann_json::nlohmann_json
    svp::core
  PRIVATE
    ${BLAKE3_LIBRARY}
)
