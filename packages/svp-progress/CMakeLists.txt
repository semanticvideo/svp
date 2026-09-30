find_package(nlohmann_json CONFIG REQUIRED)

add_library(svp-progress
  src/event.cpp
  src/fd_stream.cpp
  src/original_stderr.cpp
  src/renderer.cpp
)

add_library(svp::progress ALIAS svp-progress)

target_include_directories(svp-progress
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_compile_features(svp-progress PUBLIC cxx_std_20)

target_link_libraries(svp-progress
  PRIVATE
    nlohmann_json::nlohmann_json
)

if(BUILD_TESTING)
  add_executable(svp-progress-tests tests/renderer_tests.cpp)
  target_compile_features(svp-progress-tests PRIVATE cxx_std_20)
  target_compile_options(svp-progress-tests PRIVATE -UNDEBUG)
  target_link_libraries(svp-progress-tests PRIVATE svp::progress)
  add_test(NAME svp-progress-tests COMMAND svp-progress-tests)

  add_executable(svp-progress-fd-redirect-tests tests/fd_redirect_tests.cpp)
  target_compile_features(svp-progress-fd-redirect-tests PRIVATE cxx_std_20)
  target_compile_options(svp-progress-fd-redirect-tests PRIVATE -UNDEBUG)
  target_link_libraries(svp-progress-fd-redirect-tests PRIVATE svp::progress)
  add_test(NAME svp-progress-fd-redirect-tests
           COMMAND svp-progress-fd-redirect-tests)
endif()
