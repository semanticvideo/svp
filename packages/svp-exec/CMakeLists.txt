find_package(nlohmann_json CONFIG REQUIRED)

find_path(BLAKE3_INCLUDE_DIR NAMES blake3.h REQUIRED)
find_library(BLAKE3_LIBRARY NAMES blake3 REQUIRED)

add_library(svp-exec
  src/artifact_ref.cpp
  src/blake3_digest.cpp
  src/cache_key.cpp
  src/cancellation_token.cpp
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
    svp::models_identity
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

# --- Content-addressed store and recovery journal ----------------------------
# RC2 §20.3 (content-addressable memoization), §20.4 (recovery journal),
# §20.5 (journal cleanup, cache eviction); plan §4.6. POSIX file primitives
# (flock, fsync/F_FULLFSYNC) and SQLite.
find_package(SQLite3 REQUIRED)

target_sources(svp-exec
  PRIVATE
    src/blake3_stream.cpp
    src/cache_error.cpp
    src/cache_root.cpp
    src/cas_eviction.cpp
    src/cas_layout.cpp
    src/cas_pin.cpp
    src/cas_store.cpp
    src/durable_io.cpp
    src/file_lock.cpp
    src/journal_artifact_store.cpp
    src/journal_cleanup.cpp
    src/journal_distributed_records.cpp
    src/journal_error.cpp
    src/journal_layout.cpp
    src/journal_manifest.cpp
    src/journal_records.cpp
    src/journal_resume.cpp
    src/journal_schema.cpp
    src/journal_tasks.cpp
    src/recovery_journal.cpp
    src/source_fingerprint.cpp
    src/sqlite_database.cpp
    src/task_state.cpp
    src/utc_clock.cpp
    src/verified_blob.cpp
)

target_link_libraries(svp-exec PRIVATE SQLite::SQLite3)

if(BUILD_TESTING)
  set(SVP_EXEC_STORAGE_TESTS
    cas_store
    cas_eviction
    recovery_journal
    journal_resume
  )

  foreach(SVP_EXEC_TEST IN LISTS SVP_EXEC_STORAGE_TESTS)
    string(REPLACE "_" "-" SVP_EXEC_TEST_NAME "${SVP_EXEC_TEST}")
    add_executable(svp-exec-${SVP_EXEC_TEST_NAME}-tests
      tests/${SVP_EXEC_TEST}_tests.cpp
    )
    target_compile_features(svp-exec-${SVP_EXEC_TEST_NAME}-tests PRIVATE cxx_std_20)
    target_include_directories(svp-exec-${SVP_EXEC_TEST_NAME}-tests PRIVATE
      ${CMAKE_CURRENT_SOURCE_DIR}/tests
    )
    # Tests inspect build.sqlite directly to check the RC2 §20.4 schema.
    target_link_libraries(svp-exec-${SVP_EXEC_TEST_NAME}-tests
      PRIVATE
        svp::exec
        SQLite::SQLite3
    )
    add_test(NAME svp-exec-${SVP_EXEC_TEST_NAME}-tests
             COMMAND svp-exec-${SVP_EXEC_TEST_NAME}-tests)
  endforeach()
endif()

# ---------------------------------------------------------------------------
# Task graph, scheduler, and executors (plan §3.5, §4.3-§4.5).
# ---------------------------------------------------------------------------
find_package(Threads REQUIRED)

target_sources(svp-exec
  PRIVATE
    src/child_process.cpp
    src/clock.cpp
    src/executor.cpp
    src/fd_frame_io.cpp
    src/frame_stream.cpp
    src/in_memory_result_commit_sink.cpp
    src/in_process_executor.cpp
    src/lease_frames.cpp
    src/lease_heartbeats.cpp
    src/lease_policy.cpp
    src/loopback_executor.cpp
    src/ordered_reduction.cpp
    src/ready_set.cpp
    src/result_verification.cpp
    src/retry_policy.cpp
    src/scheduler.cpp
    src/scheduler_dispatch.cpp
    src/scheduler_inbox.cpp
    src/scheduler_names.cpp
    src/scheduler_policy.cpp
    src/scheduler_run.cpp
    src/task_attempt_runner.cpp
    src/task_graph.cpp
    src/worker_loop.cpp
    src/worker_session_leases.cpp
)

target_link_libraries(svp-exec PUBLIC Threads::Threads)

# Remote transport (plan §3.3, §3.4): Network framework TLS-PSK, Bonjour, and
# the remote executor. Apple-only; nothing above depends on it.
if(APPLE)
  add_subdirectory(remote)
endif()

if(BUILD_TESTING)
  # Toy task type and in-memory store shared by the scheduler tests and the
  # loopback test worker.
  add_library(svp-exec-toy-tasks STATIC
    tests/toy/in_memory_artifact_store.cpp
    tests/toy/toy_tasks.cpp
    tests/toy/transit_fault_writer.cpp
  )
  target_compile_features(svp-exec-toy-tasks PUBLIC cxx_std_20)
  target_include_directories(svp-exec-toy-tasks PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/toy
  )
  target_link_libraries(svp-exec-toy-tasks PUBLIC svp::exec)

  add_executable(svp-exec-test-worker tests/toy/test_worker_main.cpp)
  target_link_libraries(svp-exec-test-worker PRIVATE svp-exec-toy-tasks)
  if(APPLE)
    # --listen mode serves the worker loop over the remote transport.
    target_include_directories(svp-exec-test-worker PRIVATE
      ${CMAKE_CURRENT_SOURCE_DIR}/remote/tests
    )
    target_link_libraries(svp-exec-test-worker PRIVATE svp::exec_remote)
  endif()

  set(SVP_EXEC_SCHEDULER_TESTS
    task_graph
    scheduler
    worker_loop
    loopback_executor
  )
  foreach(SVP_EXEC_TEST IN LISTS SVP_EXEC_SCHEDULER_TESTS)
    string(REPLACE "_" "-" SVP_EXEC_TEST_NAME "${SVP_EXEC_TEST}")
    add_executable(svp-exec-${SVP_EXEC_TEST_NAME}-tests
      tests/${SVP_EXEC_TEST}_tests.cpp
    )
    target_include_directories(svp-exec-${SVP_EXEC_TEST_NAME}-tests PRIVATE
      ${CMAKE_CURRENT_SOURCE_DIR}/tests
    )
    target_link_libraries(svp-exec-${SVP_EXEC_TEST_NAME}-tests
      PRIVATE
        svp-exec-toy-tasks
    )
    add_test(NAME svp-exec-${SVP_EXEC_TEST_NAME}-tests
             COMMAND svp-exec-${SVP_EXEC_TEST_NAME}-tests
                     $<TARGET_FILE:svp-exec-test-worker>)
    set_tests_properties(svp-exec-${SVP_EXEC_TEST_NAME}-tests PROPERTIES TIMEOUT 60)
  endforeach()
  add_dependencies(svp-exec-loopback-executor-tests svp-exec-test-worker)
endif()

# ---------------------------------------------------------------------------
# Journal and cache wiring for the scheduler (plan §4.4-§4.6): the journal
# commit sink, the CAS-backed task artifact access, and scheduler resume.
# ---------------------------------------------------------------------------
target_sources(svp-exec
  PRIVATE
    src/cas_task_artifact_access.cpp
    src/committed_result_record.cpp
    src/journal_committed_results.cpp
    src/journal_result_commit_sink.cpp
    src/journal_scheduler_resume.cpp
)

if(BUILD_TESTING)
  set(SVP_EXEC_INTEGRATION_TESTS
    cas_task_artifact_access
    journal_result_commit_sink
    resume_e2e
  )
  foreach(SVP_EXEC_TEST IN LISTS SVP_EXEC_INTEGRATION_TESTS)
    string(REPLACE "_" "-" SVP_EXEC_TEST_NAME "${SVP_EXEC_TEST}")
    add_executable(svp-exec-${SVP_EXEC_TEST_NAME}-tests
      tests/${SVP_EXEC_TEST}_tests.cpp
    )
    target_include_directories(svp-exec-${SVP_EXEC_TEST_NAME}-tests PRIVATE
      ${CMAKE_CURRENT_SOURCE_DIR}/tests
    )
    # Tests inspect build.sqlite directly.
    target_link_libraries(svp-exec-${SVP_EXEC_TEST_NAME}-tests
      PRIVATE
        svp-exec-toy-tasks
        SQLite::SQLite3
    )
    add_test(NAME svp-exec-${SVP_EXEC_TEST_NAME}-tests
             COMMAND svp-exec-${SVP_EXEC_TEST_NAME}-tests
                     $<TARGET_FILE:svp-exec-test-worker>)
    set_tests_properties(svp-exec-${SVP_EXEC_TEST_NAME}-tests PROPERTIES TIMEOUT 120)
    add_dependencies(svp-exec-${SVP_EXEC_TEST_NAME}-tests svp-exec-test-worker)
  endforeach()
endif()

# ---------------------------------------------------------------------------
# Runtime identity (plan §3.2, §4.1): the runtime manifest, its runtime_id,
# and file verification. `svp-runtime-manifest` is run by `cmake --install`
# when a runtime bundle is installed (cmake/SvpInstall.cmake); it is a build
# tool, not a public CLI, so it is not installed.
# ---------------------------------------------------------------------------
target_sources(svp-exec
  PRIVATE
    src/runtime_components.cpp
    src/runtime_manifest.cpp
    src/runtime_manifest_assembly.cpp
    src/runtime_manifest_files.cpp
)

add_executable(svp-runtime-manifest tools/svp_runtime_manifest_main.cpp)
target_compile_features(svp-runtime-manifest PRIVATE cxx_std_20)
target_link_libraries(svp-runtime-manifest PRIVATE svp::exec)

if(BUILD_TESTING)
  add_executable(svp-exec-runtime-manifest-tests tests/runtime_manifest_tests.cpp)
  target_compile_features(svp-exec-runtime-manifest-tests PRIVATE cxx_std_20)
  target_include_directories(svp-exec-runtime-manifest-tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/tests
  )
  target_link_libraries(svp-exec-runtime-manifest-tests PRIVATE svp::exec)
  add_test(NAME svp-exec-runtime-manifest-tests
           COMMAND svp-exec-runtime-manifest-tests)
endif()
