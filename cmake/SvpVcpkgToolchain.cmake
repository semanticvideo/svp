if(NOT DEFINED ENV{VCPKG_ROOT} OR "$ENV{VCPKG_ROOT}" STREQUAL "")
  message(FATAL_ERROR
    "VCPKG_ROOT is not set. Bootstrap vcpkg and export VCPKG_ROOT before configuring."
  )
endif()

set(_svp_vcpkg_toolchain
  "$ENV{VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake"
)

if(NOT EXISTS "${_svp_vcpkg_toolchain}")
  message(FATAL_ERROR
    "VCPKG_ROOT does not contain a bootstrapped vcpkg checkout: $ENV{VCPKG_ROOT}"
  )
endif()

include("${_svp_vcpkg_toolchain}")
unset(_svp_vcpkg_toolchain)
