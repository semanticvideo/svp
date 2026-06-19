find_package(libzip CONFIG REQUIRED)

add_library(svp-package
  src/package_layout.cpp
  src/package_probe.cpp
)

add_library(svp::package ALIAS svp-package)

target_include_directories(svp-package
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_compile_features(svp-package PUBLIC cxx_std_20)

target_link_libraries(svp-package
  PUBLIC
    svp::core
  PRIVATE
    libzip::zip
)
