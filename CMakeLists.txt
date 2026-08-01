cmake_minimum_required(VERSION 3.24)

include(cmake/SvpVersion.cmake)

project(
  svp
  VERSION ${SVP_PROJECT_VERSION}
  DESCRIPTION "Semantic Video Package reference tools"
  LANGUAGES CXX
)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(CTest)

option(SVP_PORTABLE_TESTS_ONLY
       "Build only the platform-neutral C++ test surface"
       OFF)

if(SVP_PORTABLE_TESTS_ONLY)
  add_subdirectory(packages/svp-blocks)
  return()
endif()

include(cmake/SvpRuntimeResources.cmake)

add_subdirectory(packages/svp-core)
add_subdirectory(packages/svp-progress)
add_subdirectory(packages/svp-blocks)
add_subdirectory(packages/svp-models)
add_subdirectory(packages/svp-validation)
add_subdirectory(packages/svp-media)
add_subdirectory(packages/svp-audio)
add_subdirectory(packages/svp-vision)
add_subdirectory(packages/svp-package)
add_subdirectory(packages/svp-query)

add_subdirectory(tools/svp-validator)
add_subdirectory(tools/svp-inspector)
add_subdirectory(tools/svp-builder)
add_subdirectory(tools/svp-models-tool)

include(cmake/SvpInstall.cmake)

if(BUILD_TESTING)
  configure_file(
    "${CMAKE_SOURCE_DIR}/cmake/SvpInstallVerification.cmake.in"
    "${CMAKE_BINARY_DIR}/cmake/SvpInstallVerification.cmake"
    @ONLY
  )
  add_test(NAME svp-cli-install-uninstall-tests
    COMMAND "${CMAKE_COMMAND}"
            -P "${CMAKE_BINARY_DIR}/cmake/SvpInstallVerification.cmake"
  )
  set_tests_properties(svp-cli-install-uninstall-tests PROPERTIES RUN_SERIAL TRUE)
endif()
