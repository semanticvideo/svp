include(GNUInstallDirs)

find_program(SVP_RMDIR_EXECUTABLE NAMES rmdir REQUIRED)

set(SVP_PUBLIC_CLI_TARGETS
  svp-builder
  svp-validator
  svp-inspector
  svp-models-tool
)

foreach(target IN LISTS SVP_PUBLIC_CLI_TARGETS)
  if(NOT TARGET "${target}")
    message(FATAL_ERROR "Public CLI target does not exist: ${target}")
  endif()
endforeach()

install(TARGETS ${SVP_PUBLIC_CLI_TARGETS}
  RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
)

set(SVP_INSTALL_REGISTRY_RELATIVE_FILES
  block-types.json
  color-buckets.json
  color-spaces.json
  embedded-svpi-transport-profiles.json
  equivalence-profile.json
  ocr-observation-types.json
  reference-model-set.json
  relationship-types.json
  validation-codes.json
)
set(SVP_INSTALL_SCHEMA_RELATIVE_FILES
  color-absence.schema.json
  color-observation.schema.json
  color-summary.schema.json
  index-manifest.schema.json
  model-bundle.schema.json
  model-lock.schema.json
  numeric-value.schema.json
  signature-sidecar.schema.json
  text-absence.schema.json
  text-observation.schema.json
  text-region.schema.json
)

foreach(relative_path IN LISTS SVP_INSTALL_REGISTRY_RELATIVE_FILES)
  cmake_path(GET relative_path PARENT_PATH relative_directory)
  install(FILES "${CMAKE_SOURCE_DIR}/spec/registries/${relative_path}"
    DESTINATION "${CMAKE_INSTALL_DATADIR}/svp/registries/${relative_directory}"
  )
endforeach()

foreach(relative_path IN LISTS SVP_INSTALL_SCHEMA_RELATIVE_FILES)
  cmake_path(GET relative_path PARENT_PATH relative_directory)
  install(FILES "${CMAKE_SOURCE_DIR}/spec/schemas/${relative_path}"
    DESTINATION "${CMAKE_INSTALL_DATADIR}/svp/schemas/${relative_directory}"
  )
endforeach()

configure_file(
  "${CMAKE_SOURCE_DIR}/cmake/SvpUninstall.cmake.in"
  "${CMAKE_BINARY_DIR}/cmake/SvpUninstall.cmake"
  @ONLY
)

add_custom_target(uninstall
  COMMAND "${CMAKE_COMMAND}" -P "${CMAKE_BINARY_DIR}/cmake/SvpUninstall.cmake"
  COMMENT "Uninstalling files from the most recent CMake install"
  VERBATIM
)
