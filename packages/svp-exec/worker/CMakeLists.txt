# Worker pairing and the worker agent (plan §3.2, §3.3, §3.5, §4.1, §4.3):
# the HELLO / runtime / blob protocol, memory admission, the pairing store,
# launchd job and install script generation, runtime and model bundle
# stores, and the agent session that runs a verified runtime's session
# process. Apple-only, like the transport it serves; the parent adds this
# directory only when APPLE.

add_library(svp-exec-worker
  src/admission.cpp
  src/agent_session.cpp
  src/blob_receiver.cpp
  src/blob_source.cpp
  src/coordinator_session.cpp
  src/fleet_crypto.cpp
  src/fleet_join.cpp
  src/fleet_keys.cpp
  src/fleet_store.cpp
  src/hex_bytes.cpp
  src/hello_messages.cpp
  src/host_facts.cpp
  src/join_service.cpp
  src/launchd_job.cpp
  src/local_load.cpp
  src/message_fields.cpp
  src/model_bundles.cpp
  src/pairing_store.cpp
  src/released_blobs.cpp
  src/runtime_files.cpp
  src/runtime_source.cpp
  src/runtime_store.cpp
  src/runtime_test_start.cpp
  src/service_link.cpp
  src/service_updater.cpp
  src/session_process.cpp
  src/slot_sharing.cpp
  src/transfer_messages.cpp
  src/worker_agent.cpp
  src/worker_connection.cpp
  src/worker_error.cpp
  src/worker_layout.cpp
  src/worker_restart_wait.cpp
  src/worker_scripts.cpp
)

add_library(svp::exec_worker ALIAS svp-exec-worker)

target_include_directories(svp-exec-worker
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    # The svp-exec JSON field rules (json_fields.hpp), shared by every codec
    # in this package.
    ${CMAKE_CURRENT_SOURCE_DIR}/../src
)

target_compile_features(svp-exec-worker PUBLIC cxx_std_20)

target_link_libraries(svp-exec-worker
  PUBLIC
    svp::exec
    svp::exec_remote
    svp::models
  PRIVATE
    # P-256 signing and ECDH for fleet join (fleet_crypto.hpp).
    "-framework Security"
    "-framework CoreFoundation"
)

if(BUILD_TESTING)
  set(SVP_EXEC_WORKER_TESTS
    pairing_store
    launchd_job
    worker_protocol
    admission
    slot_sharing
    agent_session
    service_updater
    worker_restart_wait
    fleet_join
  )
  foreach(SVP_EXEC_TEST IN LISTS SVP_EXEC_WORKER_TESTS)
    string(REPLACE "_" "-" SVP_EXEC_TEST_NAME "${SVP_EXEC_TEST}")
    add_executable(svp-exec-worker-${SVP_EXEC_TEST_NAME}-tests
      tests/${SVP_EXEC_TEST}_tests.cpp
    )
    target_include_directories(svp-exec-worker-${SVP_EXEC_TEST_NAME}-tests PRIVATE
      ${CMAKE_CURRENT_SOURCE_DIR}/../tests
      ${CMAKE_CURRENT_SOURCE_DIR}/tests
    )
    target_link_libraries(svp-exec-worker-${SVP_EXEC_TEST_NAME}-tests PRIVATE
      svp::exec_worker
      svp-exec-toy-tasks
    )
    add_test(NAME svp-exec-worker-${SVP_EXEC_TEST_NAME}-tests
             COMMAND svp-exec-worker-${SVP_EXEC_TEST_NAME}-tests
                     $<TARGET_FILE:svp-exec-test-worker>)
    set_tests_properties(svp-exec-worker-${SVP_EXEC_TEST_NAME}-tests PROPERTIES TIMEOUT 120)
  endforeach()
  add_dependencies(svp-exec-worker-agent-session-tests svp-exec-test-worker)

  # Coordinator driver for a real paired worker (not a ctest: it needs a
  # worker paired with `svp-builder workers pair`). Pushes a stand-in
  # runtime whose session program is svp-exec-test-worker, runs the toy
  # graph through RemoteExecutor with the worker handshake as its session
  # preamble, and checks it is byte-identical to an in-process run.
  add_executable(svp-exec-worker-driver tests/worker_driver_main.cpp)
  target_include_directories(svp-exec-worker-driver PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../tests
    ${CMAKE_CURRENT_SOURCE_DIR}/tests
  )
  target_link_libraries(svp-exec-worker-driver PRIVATE
    svp::exec_worker
    svp-exec-toy-tasks
  )
  add_dependencies(svp-exec-worker-driver svp-exec-test-worker)
  target_compile_definitions(svp-exec-worker-driver PRIVATE
    SVP_EXEC_TEST_WORKER_PATH="$<TARGET_FILE:svp-exec-test-worker>")
endif()
