find_package(CLI11 CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)

set(SVP_INSTALL_RESOURCE_FILES
  "${CMAKE_SOURCE_DIR}/distribution/reference-models/catalog.json"
  "${CMAKE_SOURCE_DIR}/distribution/reference-models/bundle-inputs.json"
  "${CMAKE_SOURCE_DIR}/spec/registries/reference-model-set.json"
  "${CMAKE_SOURCE_DIR}/distribution/reference-models/ppocr-requirements-macos-arm64.lock"
  "${CMAKE_SOURCE_DIR}/distribution/reference-models/rfdetr-requirements-macos-arm64.lock"
  "${CMAKE_SOURCE_DIR}/scripts/model_installer/__init__.py"
  "${CMAKE_SOURCE_DIR}/scripts/model_installer/progress.py"
  "${CMAKE_SOURCE_DIR}/scripts/model_installer/downloads.py"
  "${CMAKE_SOURCE_DIR}/scripts/model_installer/conversion.py"
  "${CMAKE_SOURCE_DIR}/scripts/model_installer/workflow.py"
  "${CMAKE_SOURCE_DIR}/scripts/prepare-reference-model-bundles.py"
)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
  ${SVP_INSTALL_RESOURCE_FILES}
)

file(READ "${CMAKE_SOURCE_DIR}/distribution/reference-models/catalog.json" SVP_INSTALL_CATALOG_JSON)
file(READ "${CMAKE_SOURCE_DIR}/distribution/reference-models/bundle-inputs.json" SVP_INSTALL_BUNDLE_INPUTS_JSON)
file(READ "${CMAKE_SOURCE_DIR}/spec/registries/reference-model-set.json" SVP_INSTALL_REFERENCE_SET_JSON)
file(READ "${CMAKE_SOURCE_DIR}/distribution/reference-models/ppocr-requirements-macos-arm64.lock" SVP_INSTALL_PPOCR_LOCK)
file(READ "${CMAKE_SOURCE_DIR}/distribution/reference-models/rfdetr-requirements-macos-arm64.lock" SVP_INSTALL_RFDETR_LOCK)
file(READ "${CMAKE_SOURCE_DIR}/scripts/model_installer/__init__.py" SVP_INSTALL_INIT_PY)
file(READ "${CMAKE_SOURCE_DIR}/scripts/model_installer/progress.py" SVP_INSTALL_PROGRESS_PY)
file(READ "${CMAKE_SOURCE_DIR}/scripts/model_installer/downloads.py" SVP_INSTALL_DOWNLOADS_PY)
file(READ "${CMAKE_SOURCE_DIR}/scripts/model_installer/conversion.py" SVP_INSTALL_CONVERSION_PY)
file(READ "${CMAKE_SOURCE_DIR}/scripts/model_installer/workflow.py" SVP_INSTALL_WORKFLOW_PY)
file(READ "${CMAKE_SOURCE_DIR}/scripts/prepare-reference-model-bundles.py" SVP_INSTALL_PREPARE_PY)

configure_file(
  src/install_resources.cpp.in
  ${CMAKE_CURRENT_BINARY_DIR}/generated/install_resources.cpp
  @ONLY
)

add_executable(svp-models-tool
  src/main.cpp
  src/install_command.cpp
  src/install_process.cpp
  ${CMAKE_CURRENT_BINARY_DIR}/generated/install_resources.cpp
)

target_compile_features(svp-models-tool PRIVATE cxx_std_20)

target_include_directories(svp-models-tool PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(svp-models-tool
  PRIVATE
    CLI11::CLI11
    nlohmann_json::nlohmann_json
    svp::models
    svp::progress
    svp::core
)

if(BUILD_TESTING)
  find_package(Python3 COMPONENTS Interpreter)
  add_test(NAME svp-models-tool-manifest-help
           COMMAND svp-models-tool verify --help)
  set_tests_properties(svp-models-tool-manifest-help PROPERTIES
    PASS_REGULAR_EXPRESSION "verifies its complete containing")

  add_test(NAME svp-models-tool-manifest-name
           COMMAND svp-models-tool verify --manifest not-a-manifest.json)
  set_tests_properties(svp-models-tool-manifest-name PROPERTIES
    WILL_FAIL TRUE)

  add_test(NAME svp-models-tool-hash-help
           COMMAND svp-models-tool hash --help)
  set_tests_properties(svp-models-tool-hash-help PROPERTIES
    PASS_REGULAR_EXPRESSION "File to hash")

  add_test(NAME svp-models-tool-digest-help
           COMMAND svp-models-tool digest --help)
  set_tests_properties(svp-models-tool-digest-help PROPERTIES
    PASS_REGULAR_EXPRESSION "canonical BLAKE3 digest")

  add_test(NAME svp-models-tool-install-help
           COMMAND svp-models-tool install --help)
  set_tests_properties(svp-models-tool-install-help PROPERTIES
    PASS_REGULAR_EXPRESSION "Concurrent model jobs")

  add_test(NAME svp-models-tool-path-default COMMAND svp-models-tool path)
  if(APPLE)
    set(SVP_MODELS_DEFAULT_PATH_PATTERN
        "Library/Application Support/SVP/Models/v1")
  elseif(WIN32)
    set(SVP_MODELS_DEFAULT_PATH_PATTERN "SVP.*Cache.*v1.*models")
  else()
    set(SVP_MODELS_DEFAULT_PATH_PATTERN "[.]cache/svp/v1/models")
  endif()
  set_tests_properties(svp-models-tool-path-default PROPERTIES
    PASS_REGULAR_EXPRESSION "${SVP_MODELS_DEFAULT_PATH_PATTERN}")

  add_executable(svp-models-executable-path-tests
    tests/executable_path_tests.cpp
  )
  target_compile_features(svp-models-executable-path-tests PRIVATE cxx_std_20)
  target_link_libraries(svp-models-executable-path-tests PRIVATE
    svp::core
  )
  add_test(NAME svp-models-executable-path-tests
           COMMAND svp-models-executable-path-tests)

  add_executable(svp-models-install-process-tests
    tests/install_process_tests.cpp
    src/install_process.cpp
  )
  target_compile_features(svp-models-install-process-tests PRIVATE cxx_std_20)
  target_include_directories(svp-models-install-process-tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
  )
  add_test(NAME svp-models-install-process-tests
           COMMAND svp-models-install-process-tests)

  if(Python3_Interpreter_FOUND)
    add_test(
      NAME svp-model-installer-tests
      COMMAND ${Python3_EXECUTABLE} -m unittest discover
              -s scripts/model_installer/tests
      WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    )
    set_tests_properties(svp-model-installer-tests PROPERTIES
      ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1"
    )
  endif()
endif()
