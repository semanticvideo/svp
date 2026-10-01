# Remote transport for svp-exec (plan §3.3, §3.4, §4.3): the worker listener
# and coordinator connector over Network framework TLS 1.2 PSK with Bonjour
# discovery, and the RemoteExecutor. Apple-only (the parent adds this
# directory only when APPLE); the portable engine never depends on it.

add_library(svp-exec-remote
  src/bonjour_browser.cpp
  src/interface_link_rate.cpp
  src/nw_stream.cpp
  src/pairing_key.cpp
  src/remote_connector.cpp
  src/remote_error.cpp
  src/remote_executor.cpp
  src/remote_listener.cpp
  src/route_measurement.cpp
  src/route_policy.cpp
  src/route_probe_responder.cpp
  src/service_resolver.cpp
  src/tls_psk_parameters.cpp
  src/transport_policy.cpp
)

add_library(svp::exec_remote ALIAS svp-exec-remote)

target_include_directories(svp-exec-remote
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_compile_features(svp-exec-remote PUBLIC cxx_std_20)
# Network framework callbacks are blocks.
target_compile_options(svp-exec-remote PRIVATE -fblocks)

target_link_libraries(svp-exec-remote
  PUBLIC
    svp::exec
  PRIVATE
    "-framework Network"
    "-framework Security"
    "-framework CoreFoundation"
)

if(BUILD_TESTING)
  add_executable(svp-exec-route-policy-tests tests/route_policy_tests.cpp)
  target_include_directories(svp-exec-route-policy-tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../tests
  )
  target_link_libraries(svp-exec-route-policy-tests PRIVATE svp::exec_remote)
  add_test(NAME svp-exec-route-policy-tests COMMAND svp-exec-route-policy-tests)

  # These open TLS listeners and browse Bonjour on this Mac (loopback and
  # whatever interfaces the system advertises on). Each run uses fresh random
  # pairing ids, so parallel runs never find each other's workers.
  add_executable(svp-exec-remote-transport-tests tests/remote_transport_tests.cpp)
  target_include_directories(svp-exec-remote-transport-tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../tests
    ${CMAKE_CURRENT_SOURCE_DIR}/tests
  )
  target_link_libraries(svp-exec-remote-transport-tests PRIVATE svp::exec_remote)
  add_test(NAME svp-exec-remote-transport-tests COMMAND svp-exec-remote-transport-tests)
  set_tests_properties(svp-exec-remote-transport-tests PROPERTIES TIMEOUT 120 LABELS network)

  add_executable(svp-exec-remote-executor-tests tests/remote_executor_tests.cpp)
  target_include_directories(svp-exec-remote-executor-tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../tests
    ${CMAKE_CURRENT_SOURCE_DIR}/tests
  )
  target_link_libraries(svp-exec-remote-executor-tests PRIVATE
    svp::exec_remote
    svp-exec-toy-tasks
  )
  add_dependencies(svp-exec-remote-executor-tests svp-exec-test-worker)
  add_test(NAME svp-exec-remote-executor-tests
           COMMAND svp-exec-remote-executor-tests $<TARGET_FILE:svp-exec-test-worker>)
  set_tests_properties(svp-exec-remote-executor-tests PROPERTIES TIMEOUT 180 LABELS network)

  # Coordinator driver for cross-machine runs against test workers started
  # with --listen (not a ctest: it needs workers on other Macs).
  add_executable(svp-exec-remote-driver tests/remote_driver_main.cpp)
  target_include_directories(svp-exec-remote-driver PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../tests
    ${CMAKE_CURRENT_SOURCE_DIR}/tests
  )
  target_link_libraries(svp-exec-remote-driver PRIVATE
    svp::exec_remote
    svp-exec-toy-tasks
  )
endif()
