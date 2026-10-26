find_package(nlohmann_json CONFIG REQUIRED)

add_library(svp-media
  src/canonical_raster.cpp
  src/canonical_timing.cpp
  src/capture_location.cpp
  src/capture_metadata.cpp
  src/capture_time.cpp
  src/isobmff_meta_xml.cpp
  src/media_ingest_plan.cpp
  src/media_probe.cpp
  src/rational.cpp
  src/sony_nonrealtime_meta.cpp
)

add_library(svp::media ALIAS svp-media)

target_include_directories(svp-media
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_compile_features(svp-media PUBLIC cxx_std_20)

target_link_libraries(svp-media
  PUBLIC
    nlohmann_json::nlohmann_json
)

if(BUILD_TESTING)
  add_executable(svp-media-tests
    tests/media_timing_tests.cpp
  )

  target_compile_features(svp-media-tests PRIVATE cxx_std_20)

  target_link_libraries(svp-media-tests
    PRIVATE
      svp::media
  )

  add_test(NAME svp-media-tests COMMAND svp-media-tests)

  add_executable(svp-media-capture-tests
    tests/capture_metadata_tests.cpp
  )

  target_compile_features(svp-media-capture-tests PRIVATE cxx_std_20)

  target_link_libraries(svp-media-capture-tests
    PRIVATE
      svp::media
  )

  add_test(NAME svp-media-capture-tests COMMAND svp-media-capture-tests)
endif()
