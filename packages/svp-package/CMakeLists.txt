find_package(libzip CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)

add_library(svp-package
  src/package_contract.cpp
  src/package_layout.cpp
  src/package_probe.cpp
  src/package_summary.cpp
  src/package_writer.cpp
)

add_library(svp::package ALIAS svp-package)

target_include_directories(svp-package
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_compile_features(svp-package PUBLIC cxx_std_20)

target_link_libraries(svp-package
  PUBLIC
    nlohmann_json::nlohmann_json
    svp::core
  PRIVATE
    libzip::zip
)

if(BUILD_TESTING)
  add_executable(svp-package-tests
    tests/package_writer_tests.cpp
  )

  target_compile_features(svp-package-tests PRIVATE cxx_std_20)

  target_link_libraries(svp-package-tests
    PRIVATE
      svp::package
  )

  add_test(NAME svp-package-tests COMMAND svp-package-tests)
endif()
