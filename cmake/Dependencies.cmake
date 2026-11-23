# SPDX-License-Identifier: Apache-2.0
#
# Pinned third-party dependencies (ADR-0018): exact release archives verified by SHA-256.
# System copies are never used. For offline builds put the archives into YAP_DEPS_DIR
# under the file names below; hashes are verified the same way.
include(FetchContent)
include(ExternalProject)

set(YAP_DEPS_DIR "" CACHE PATH "Directory with pre-downloaded dependency archives (offline build)")

function(yap_dep_url out_var file_name url)
  if(YAP_DEPS_DIR)
    set(${out_var} "file://${YAP_DEPS_DIR}/${file_name}" PARENT_SCOPE)
  else()
    set(${out_var} "${url}" PARENT_SCOPE)
  endif()
endfunction()

# ------------------------------------------------------------------ libsodium 1.0.22
# Release signed with the libsodium minisign key
# RWQf6LRCGA9i53mlYecO4IzT51TGPpvWucNSCh1CBM0QTaLn73Y7GFO3 (signature verified when pinning).
set(YAP_LIBSODIUM_VERSION 1.0.22)
set(YAP_LIBSODIUM_SHA256 adbdd8f16149e81ac6078a03aca6fc03b592b89ef7b5ed83841c086191be3349)
yap_dep_url(_yap_sodium_url "libsodium-${YAP_LIBSODIUM_VERSION}.tar.gz"
  "https://github.com/jedisct1/libsodium/releases/download/${YAP_LIBSODIUM_VERSION}-RELEASE/libsodium-${YAP_LIBSODIUM_VERSION}.tar.gz")

set(_yap_sodium_prefix "${CMAKE_BINARY_DIR}/_deps/libsodium-install")
cmake_host_system_information(RESULT _yap_jobs QUERY NUMBER_OF_LOGICAL_CORES)
find_program(YAP_MAKE_PROGRAM NAMES gmake make REQUIRED)

ExternalProject_Add(yap_libsodium_build
  URL "${_yap_sodium_url}"
  URL_HASH SHA256=${YAP_LIBSODIUM_SHA256}
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE
  PREFIX "${CMAKE_BINARY_DIR}/_deps/libsodium"
  INSTALL_DIR "${_yap_sodium_prefix}"
  CONFIGURE_COMMAND <SOURCE_DIR>/configure
    --prefix=<INSTALL_DIR>
    --libdir=<INSTALL_DIR>/lib
    --disable-shared --enable-static --with-pic
    --disable-dependency-tracking
    "CC=${CMAKE_C_COMPILER}"
  BUILD_COMMAND ${YAP_MAKE_PROGRAM} -j${_yap_jobs}
  INSTALL_COMMAND ${YAP_MAKE_PROGRAM} install
  BUILD_BYPRODUCTS "${_yap_sodium_prefix}/lib/libsodium.a"
  LOG_DOWNLOAD ON LOG_CONFIGURE ON LOG_BUILD ON LOG_INSTALL ON LOG_OUTPUT_ON_FAILURE ON)

file(MAKE_DIRECTORY "${_yap_sodium_prefix}/include")
add_library(yap_sodium STATIC IMPORTED GLOBAL)
set_target_properties(yap_sodium PROPERTIES
  IMPORTED_LOCATION "${_yap_sodium_prefix}/lib/libsodium.a"
  INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${_yap_sodium_prefix}/include"
  INTERFACE_INCLUDE_DIRECTORIES "${_yap_sodium_prefix}/include")
add_dependencies(yap_sodium yap_libsodium_build)
add_library(yap::sodium ALIAS yap_sodium)

# ------------------------------------------------------------------ nlohmann/json 3.12.0
# Used by the core only to parse the genesis JSON (protocol.md §11.3); never for hashing.
set(YAP_JSON_VERSION 3.12.0)
set(YAP_JSON_SHA256 42f6e95cad6ec532fd372391373363b62a14af6d771056dbfc86160e6dfff7aa)
yap_dep_url(_yap_json_url "json-${YAP_JSON_VERSION}.tar.xz"
  "https://github.com/nlohmann/json/releases/download/v${YAP_JSON_VERSION}/json.tar.xz")
FetchContent_Declare(nlohmann_json
  URL "${_yap_json_url}"
  URL_HASH SHA256=${YAP_JSON_SHA256}
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE
  SYSTEM)
set(JSON_Install OFF CACHE BOOL "" FORCE)
set(JSON_BuildTests OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(nlohmann_json)

# ------------------------------------------------------------------ test-only dependencies
if(YAP_BUILD_TESTS OR YAP_FUZZ)
  set(YAP_CATCH2_VERSION 3.16.1)
  set(YAP_CATCH2_SHA256 d16bd1b8b2364918bd472cb216394acb93bde6784890df0d91bccfb3f28d777d)
  yap_dep_url(_yap_catch2_url "catch2-${YAP_CATCH2_VERSION}.tar.gz"
    "https://github.com/catchorg/Catch2/archive/refs/tags/v${YAP_CATCH2_VERSION}.tar.gz")
  FetchContent_Declare(Catch2
    URL "${_yap_catch2_url}"
    URL_HASH SHA256=${YAP_CATCH2_SHA256}
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    SYSTEM)

  set(CATCH_INSTALL_DOCS OFF CACHE BOOL "" FORCE)
  set(CATCH_INSTALL_EXTRAS OFF CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(Catch2)
  list(APPEND CMAKE_MODULE_PATH "${catch2_SOURCE_DIR}/extras")
endif()
