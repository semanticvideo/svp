set(SVP_RUNTIME_RESOURCE_ROOT "${CMAKE_BINARY_DIR}/tools/share/svp")

file(GLOB_RECURSE SVP_RUNTIME_RESOURCE_INPUTS CONFIGURE_DEPENDS
  "${CMAKE_SOURCE_DIR}/spec/registries/*"
  "${CMAKE_SOURCE_DIR}/spec/schemas/*"
)

add_custom_target(svp-runtime-resources
  COMMAND ${CMAKE_COMMAND} -E remove_directory
          "${SVP_RUNTIME_RESOURCE_ROOT}"
  COMMAND ${CMAKE_COMMAND} -E make_directory
          "${SVP_RUNTIME_RESOURCE_ROOT}"
  COMMAND ${CMAKE_COMMAND} -E copy_directory
          "${CMAKE_SOURCE_DIR}/spec/registries"
          "${SVP_RUNTIME_RESOURCE_ROOT}/registries"
  COMMAND ${CMAKE_COMMAND} -E copy_directory
          "${CMAKE_SOURCE_DIR}/spec/schemas"
          "${SVP_RUNTIME_RESOURCE_ROOT}/schemas"
  DEPENDS ${SVP_RUNTIME_RESOURCE_INPUTS}
  COMMENT "Staging SVP runtime registries and schemas"
  VERBATIM
)
