find_package(nlohmann_json CONFIG REQUIRED)

add_library(svp-audio
  src/audio_stage_plan.cpp
  src/transcript_records.cpp
)

add_library(svp::audio ALIAS svp-audio)

target_include_directories(svp-audio
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_compile_features(svp-audio PUBLIC cxx_std_20)

target_link_libraries(svp-audio
  PUBLIC
    nlohmann_json::nlohmann_json
    svp::media
)

if(BUILD_TESTING)
  add_executable(svp-audio-tests
    tests/audio_transcript_tests.cpp
  )

  target_compile_features(svp-audio-tests PRIVATE cxx_std_20)

  target_link_libraries(svp-audio-tests
    PRIVATE
      svp::audio
  )

  add_test(NAME svp-audio-tests COMMAND svp-audio-tests)
endif()
