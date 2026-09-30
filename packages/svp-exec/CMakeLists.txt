find_package(nlohmann_json CONFIG REQUIRED)

find_path(BLAKE3_INCLUDE_DIR NAMES blake3.h REQUIRED)
find_library(BLAKE3_LIBRARY NAMES blake3 REQUIRED)

add_library(svp-exec
  src/artifact_ref.cpp
  src/blake3_digest.cpp
  src/cache_key.cpp
  src/canonical_json.cpp
  src/exec_error.cpp
  src/frame_decoder.cpp
  src/frame_encoder.cpp
  src/frame_header.cpp
  src/json_fields.cpp
  src/message_type.cpp
  src/output_digest.cpp
  src/parameters_digest.cpp
  src/processor_cache_keys.cpp
  src/record_identifiers.cpp
  src/task_execution.cpp
  src/task_frames.cpp
  src/task_registry.cpp
  src/task_result.cpp
  src/task_spec.cpp
)

add_library(svp::exec ALIAS svp-exec)

target_include_directories(svp-exec
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
  PRIVATE
    ${BLAKE3_INCLUDE_DIR}
)

target_compile_features(svp-exec PUBLIC cxx_std_20)

target_link_libraries(svp-exec
  PUBLIC
    nlohmann_json::nlohmann_json
  PRIVATE
    svp::core
    svp::models
    ${BLAKE3_LIBRARY}
)

if(BUILD_TESTING)
  set(SVP_EXEC_TESTS
    canonical_json
    records
    digests
    framing
    task_registry
  )

  foreach(SVP_EXEC_TEST IN LISTS SVP_EXEC_TESTS)
    string(REPLACE "_" "-" SVP_EXEC_TEST_NAME "${SVP_EXEC_TEST}")
    add_executable(svp-exec-${SVP_EXEC_TEST_NAME}-tests
      tests/${SVP_EXEC_TEST}_tests.cpp
    )
    target_compile_features(svp-exec-${SVP_EXEC_TEST_NAME}-tests PRIVATE cxx_std_20)
    target_include_directories(svp-exec-${SVP_EXEC_TEST_NAME}-tests PRIVATE
      ${CMAKE_CURRENT_SOURCE_DIR}/tests
    )
    target_link_libraries(svp-exec-${SVP_EXEC_TEST_NAME}-tests
      PRIVATE
        svp::exec
    )
    add_test(NAME svp-exec-${SVP_EXEC_TEST_NAME}-tests
             COMMAND svp-exec-${SVP_EXEC_TEST_NAME}-tests)
  endforeach()
endif()
