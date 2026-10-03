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

file(GLOB_RECURSE SVP_INSTALL_REGISTRY_RELATIVE_FILES
  CONFIGURE_DEPENDS
  LIST_DIRECTORIES false
  RELATIVE "${CMAKE_SOURCE_DIR}/spec/registries"
  "${CMAKE_SOURCE_DIR}/spec/registries/*.json"
)
file(GLOB_RECURSE SVP_INSTALL_SCHEMA_RELATIVE_FILES
  CONFIGURE_DEPENDS
  LIST_DIRECTORIES false
  RELATIVE "${CMAKE_SOURCE_DIR}/spec/schemas"
  "${CMAKE_SOURCE_DIR}/spec/schemas/*.json"
)
list(SORT SVP_INSTALL_REGISTRY_RELATIVE_FILES)
list(SORT SVP_INSTALL_SCHEMA_RELATIVE_FILES)

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

# --- Optional runtime bundle ----------------------------------------------------
# A runtime bundle built by distribution/runtime-bundle (pinned ffmpeg, ffprobe,
# sherpa-onnx, and ONNX Runtime) is installed only when this variable names
# its output directory. Left empty, the install is exactly the CLI tools and
# resources above, and svp-builder uses tools from PATH as before.
set(SVP_RUNTIME_BUNDLE_DIR "" CACHE PATH
  "Built runtime bundle directory to install into <prefix>/libexec/svp/runtime (empty: none)")

set(SVP_INSTALL_RUNTIME_RELATIVE_FILES)
set(SVP_RUNTIME_INSTALL_DIR "")
if(NOT SVP_RUNTIME_BUNDLE_DIR STREQUAL "")
  # svp-builder looks for the bundle at <real exe dir>/../libexec/svp/runtime
  # (kRuntimeBundleDirFromExecutableDir in svp/builder/runtime_tools.hpp), so
  # the bundle can only be installed with the default bin/libexec layout.
  if(NOT CMAKE_INSTALL_BINDIR STREQUAL "bin" OR
     NOT CMAKE_INSTALL_LIBEXECDIR STREQUAL "libexec")
    message(FATAL_ERROR
      "SVP_RUNTIME_BUNDLE_DIR requires CMAKE_INSTALL_BINDIR=bin and "
      "CMAKE_INSTALL_LIBEXECDIR=libexec; svp-builder finds the bundle at "
      "../libexec/svp/runtime relative to itself."
    )
  endif()
  set(SVP_RUNTIME_INSTALL_DIR "${CMAKE_INSTALL_LIBEXECDIR}/svp/runtime")

  set(svp_runtime_components "${SVP_RUNTIME_BUNDLE_DIR}/components.json")
  if(NOT EXISTS "${svp_runtime_components}")
    message(FATAL_ERROR
      "SVP_RUNTIME_BUNDLE_DIR has no components.json: ${SVP_RUNTIME_BUNDLE_DIR}\n"
      "Build the bundle and run distribution/runtime-bundle/write-components-manifest.sh first."
    )
  endif()
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${svp_runtime_components}"
  )

  # The executables and libraries are exactly the files components.json
  # declares; the license texts ship with them for redistribution.
  file(READ "${svp_runtime_components}" svp_runtime_components_json)
  string(JSON svp_runtime_component_count
    LENGTH "${svp_runtime_components_json}" components)
  if(svp_runtime_component_count EQUAL 0)
    message(FATAL_ERROR "components.json declares no components: ${svp_runtime_components}")
  endif()
  math(EXPR svp_runtime_last_component "${svp_runtime_component_count} - 1")
  foreach(component_index RANGE ${svp_runtime_last_component})
    string(JSON svp_runtime_file_count
      LENGTH "${svp_runtime_components_json}" components ${component_index} files)
    if(svp_runtime_file_count EQUAL 0)
      continue()
    endif()
    math(EXPR svp_runtime_last_file "${svp_runtime_file_count} - 1")
    foreach(file_index RANGE ${svp_runtime_last_file})
      string(JSON relative_path GET "${svp_runtime_components_json}"
        components ${component_index} files ${file_index} path)
      if(NOT EXISTS "${SVP_RUNTIME_BUNDLE_DIR}/${relative_path}")
        message(FATAL_ERROR
          "components.json lists a file the bundle does not contain: "
          "${SVP_RUNTIME_BUNDLE_DIR}/${relative_path}"
        )
      endif()
      list(APPEND SVP_INSTALL_RUNTIME_RELATIVE_FILES "${relative_path}")
    endforeach()
  endforeach()
  file(GLOB_RECURSE svp_runtime_license_files
    LIST_DIRECTORIES false
    RELATIVE "${SVP_RUNTIME_BUNDLE_DIR}"
    "${SVP_RUNTIME_BUNDLE_DIR}/licenses/*"
  )
  list(APPEND SVP_INSTALL_RUNTIME_RELATIVE_FILES
    ${svp_runtime_license_files}
    components.json
  )
  list(REMOVE_DUPLICATES SVP_INSTALL_RUNTIME_RELATIVE_FILES)
  list(SORT SVP_INSTALL_RUNTIME_RELATIVE_FILES)

  foreach(relative_path IN LISTS SVP_INSTALL_RUNTIME_RELATIVE_FILES)
    cmake_path(GET relative_path PARENT_PATH relative_directory)
    set(svp_runtime_destination "${SVP_RUNTIME_INSTALL_DIR}/${relative_directory}")
    if(relative_path MATCHES "^bin/")
      install(PROGRAMS "${SVP_RUNTIME_BUNDLE_DIR}/${relative_path}"
        DESTINATION "${svp_runtime_destination}"
      )
    else()
      install(FILES "${SVP_RUNTIME_BUNDLE_DIR}/${relative_path}"
        DESTINATION "${svp_runtime_destination}"
      )
    endif()
  endforeach()

  # A worker that coordinates a whole video (build-batch) runs the full build,
  # including strict validation, which needs the registries and schemas, so
  # they are part of the runtime a coordinator sends its workers.
  set(svp_runtime_data_arguments "")
  foreach(relative_path IN LISTS SVP_INSTALL_REGISTRY_RELATIVE_FILES)
    list(APPEND svp_runtime_data_arguments --add
         "svp-data=${CMAKE_INSTALL_DATADIR}/svp/registries/${relative_path}")
  endforeach()
  foreach(relative_path IN LISTS SVP_INSTALL_SCHEMA_RELATIVE_FILES)
    list(APPEND svp_runtime_data_arguments --add
         "svp-data=${CMAKE_INSTALL_DATADIR}/svp/schemas/${relative_path}")
  endforeach()

  # The runtime identity covers the installed svp-builder as well as the
  # bundle, so it is written after both are in place. svp-runtime-manifest
  # also checks every bundled file against its components.json digest.
  install(CODE "
    set(svp_runtime_data_arguments \"${svp_runtime_data_arguments}\")
    set(svp_runtime_root \"\$ENV{DESTDIR}\${CMAKE_INSTALL_PREFIX}\")
    execute_process(
      COMMAND \"$<TARGET_FILE:svp-runtime-manifest>\" write
              --root \"\${svp_runtime_root}\"
              --bundle-dir \"${SVP_RUNTIME_INSTALL_DIR}\"
              --add \"svp-builder=${CMAKE_INSTALL_BINDIR}/$<TARGET_FILE_NAME:svp-builder>\"
              \${svp_runtime_data_arguments}
      RESULT_VARIABLE svp_runtime_manifest_result
      OUTPUT_VARIABLE svp_runtime_id
      ERROR_VARIABLE svp_runtime_manifest_error
      OUTPUT_STRIP_TRAILING_WHITESPACE
    )
    if(NOT svp_runtime_manifest_result EQUAL 0)
      message(FATAL_ERROR \"Writing the runtime manifest failed: \${svp_runtime_manifest_error}\")
    endif()
    list(APPEND CMAKE_INSTALL_MANIFEST_FILES
      \"\${CMAKE_INSTALL_PREFIX}/${SVP_RUNTIME_INSTALL_DIR}/manifest.json\")
    message(STATUS \"Installed runtime manifest (runtime_id \${svp_runtime_id})\")
  ")
endif()

# The install verification test (SvpInstallVerification.cmake.in) runs the
# manifest verifier; configure_file cannot expand generator expressions.
set(SVP_RUNTIME_MANIFEST_TOOL_FILE "${CMAKE_BINARY_DIR}/cmake/svp-runtime-manifest-path.txt")
file(GENERATE
  OUTPUT "${SVP_RUNTIME_MANIFEST_TOOL_FILE}"
  CONTENT "$<TARGET_FILE:svp-runtime-manifest>"
)

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
