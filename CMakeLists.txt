cmake_minimum_required(VERSION 3.24)

project(
  svp
  VERSION 1.0.0
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

add_subdirectory(packages/svp-core)
add_subdirectory(packages/svp-blocks)
add_subdirectory(packages/svp-models)
add_subdirectory(packages/svp-validation)
add_subdirectory(packages/svp-media)
add_subdirectory(packages/svp-audio)
add_subdirectory(packages/svp-vision)
add_subdirectory(packages/svp-package)
add_subdirectory(packages/svp-query)

add_subdirectory(tools/svp-validator)
add_subdirectory(tools/svp-reader)
add_subdirectory(tools/svp-inspector)
add_subdirectory(tools/svp-builder)
add_subdirectory(tools/svp-models-tool)
