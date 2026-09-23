# Find the pinned wgpu-native shared library.
#
# Set WGPU_NATIVE_ROOT to the root of a wgpu-native package containing:
#   include/webgpu/webgpu.h
#   include/webgpu/wgpu.h
#   lib/<wgpu-native shared library and, on Windows, import library>
#
# This module defines:
#   WgpuNative_FOUND
#   WGPU_NATIVE_INCLUDE_DIR
#   WGPU_NATIVE_LIBRARY
#   WGPU_NATIVE_RUNTIME_LIBRARY
#   WGPU::Native

set(WGPU_NATIVE_ROOT "${PROJECT_SOURCE_DIR}/ddnet-libs/wgpu" CACHE PATH "Path to the pinned wgpu-native package")
set(WGPU_NATIVE_EXPECTED_VERSION "v29.0.1.1")

set(WGPU_NATIVE_VERSION_FILE "${WGPU_NATIVE_ROOT}/wgpu-native-meta/wgpu-native-git-tag")
if(EXISTS "${WGPU_NATIVE_VERSION_FILE}")
  file(STRINGS "${WGPU_NATIVE_VERSION_FILE}" WGPU_NATIVE_VERSION LIMIT_COUNT 1)
endif()

# The package is fetched to a host path and is not part of any sysroot, so
# every search below has to be taken literally. NO_DEFAULT_PATH is not enough
# for that: a toolchain that fills CMAKE_FIND_ROOT_PATH and sets the INCLUDE
# and LIBRARY modes to ONLY - the Android NDK does both - re-roots even the
# paths given here, and the search then looks for the package inside the NDK.
# NO_CMAKE_FIND_ROOT_PATH is what turns that off per call. The version above
# is read with file(), which is never re-rooted, so it is found either way.
find_path(WGPU_NATIVE_INCLUDE_DIR
  NAMES webgpu/webgpu.h
  PATHS "${WGPU_NATIVE_ROOT}/include"
  NO_DEFAULT_PATH
  NO_CMAKE_FIND_ROOT_PATH
)
if(WGPU_NATIVE_INCLUDE_DIR AND NOT EXISTS "${WGPU_NATIVE_INCLUDE_DIR}/webgpu/wgpu.h")
  set(WGPU_NATIVE_INCLUDE_DIR WGPU_NATIVE_INCLUDE_DIR-NOTFOUND)
endif()

if(WIN32)
  if(MINGW)
    set(WGPU_NATIVE_IMPORT_LIBRARY_NAME libwgpu_native.dll.a)
  else()
    set(WGPU_NATIVE_IMPORT_LIBRARY_NAME wgpu_native.dll.lib)
  endif()
  find_file(WGPU_NATIVE_LIBRARY
    NAMES ${WGPU_NATIVE_IMPORT_LIBRARY_NAME}
    PATHS "${WGPU_NATIVE_ROOT}/lib"
    NO_DEFAULT_PATH
    NO_CMAKE_FIND_ROOT_PATH
  )
  find_file(WGPU_NATIVE_RUNTIME_LIBRARY
    NAMES wgpu_native.dll
    PATHS
      "${WGPU_NATIVE_ROOT}/bin"
      "${WGPU_NATIVE_ROOT}/lib"
    NO_DEFAULT_PATH
    NO_CMAKE_FIND_ROOT_PATH
  )
elseif(APPLE)
  find_file(WGPU_NATIVE_RUNTIME_LIBRARY
    NAMES libwgpu_native.dylib
    PATHS "${WGPU_NATIVE_ROOT}/lib"
    NO_DEFAULT_PATH
    NO_CMAKE_FIND_ROOT_PATH
  )
  set(WGPU_NATIVE_LIBRARY "${WGPU_NATIVE_RUNTIME_LIBRARY}")
else()
  find_file(WGPU_NATIVE_RUNTIME_LIBRARY
    NAMES libwgpu_native.so
    PATHS "${WGPU_NATIVE_ROOT}/lib"
    NO_DEFAULT_PATH
    NO_CMAKE_FIND_ROOT_PATH
  )
  set(WGPU_NATIVE_LIBRARY "${WGPU_NATIVE_RUNTIME_LIBRARY}")
endif()

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(WgpuNative
  REQUIRED_VARS
    WGPU_NATIVE_INCLUDE_DIR
    WGPU_NATIVE_LIBRARY
    WGPU_NATIVE_RUNTIME_LIBRARY
  VERSION_VAR WGPU_NATIVE_VERSION
  REASON_FAILURE_MESSAGE "Run python scripts/fetch_wgpu_native.py or set WGPU_NATIVE_ROOT to the pinned release package"
)

if(WgpuNative_FOUND AND NOT WGPU_NATIVE_VERSION STREQUAL WGPU_NATIVE_EXPECTED_VERSION)
  message(FATAL_ERROR "WebGPU support requires wgpu-native ${WGPU_NATIVE_EXPECTED_VERSION}, found ${WGPU_NATIVE_VERSION}")
endif()

if(WgpuNative_FOUND AND NOT TARGET WGPU::Native)
  add_library(WGPU::Native SHARED IMPORTED)
  set_target_properties(WGPU::Native PROPERTIES
    IMPORTED_LOCATION "${WGPU_NATIVE_RUNTIME_LIBRARY}"
    INTERFACE_INCLUDE_DIRECTORIES "${WGPU_NATIVE_INCLUDE_DIR}"
  )
  if(WIN32)
    set_property(TARGET WGPU::Native PROPERTY IMPORTED_IMPLIB "${WGPU_NATIVE_LIBRARY}")
  elseif(NOT APPLE)
    set_property(TARGET WGPU::Native PROPERTY IMPORTED_NO_SONAME TRUE)
  endif()
endif()

mark_as_advanced(
  WGPU_NATIVE_INCLUDE_DIR
  WGPU_NATIVE_IMPORT_LIBRARY_NAME
  WGPU_NATIVE_LIBRARY
  WGPU_NATIVE_RUNTIME_LIBRARY
  WGPU_NATIVE_VERSION_FILE
)
