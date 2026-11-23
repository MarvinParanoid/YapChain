# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.25)

project(YapChain
  VERSION 0.1.0
  DESCRIPTION "YapChain: the economy is real, the value is questionable"
  LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
endif()

option(YAP_BUILD_TESTS "Build unit and vector tests" ON)
option(YAP_WERROR "Treat warnings as errors" ON)
option(YAP_SANITIZE "Build with AddressSanitizer + UndefinedBehaviorSanitizer" OFF)
option(YAP_FUZZ "Build libFuzzer targets (Clang only)" OFF)
option(YAP_ALLOW_UNSUPPORTED_PLATFORM "Allow configuring on platforms other than Linux x86_64/aarch64" OFF)

# ADR-0012: supported platforms are Linux x86_64 and Linux aarch64.
if(NOT YAP_ALLOW_UNSUPPORTED_PLATFORM)
  if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux" OR NOT CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|aarch64|AMD64|arm64)$")
    message(FATAL_ERROR
      "Unsupported platform ${CMAKE_SYSTEM_NAME}/${CMAKE_SYSTEM_PROCESSOR}. "
      "Supported: Linux x86_64, Linux aarch64. Override with -DYAP_ALLOW_UNSUPPORTED_PLATFORM=ON.")
  endif()
endif()

if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 13)
  message(FATAL_ERROR "GCC >= 13 required")
elseif(CMAKE_CXX_COMPILER_ID STREQUAL "Clang" AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 17)
  message(FATAL_ERROR "Clang >= 17 required")
elseif(NOT CMAKE_CXX_COMPILER_ID MATCHES "^(GNU|Clang)$")
  message(FATAL_ERROR "Supported compilers: GCC >= 13, Clang >= 17")
endif()

list(APPEND CMAKE_MODULE_PATH ${PROJECT_SOURCE_DIR}/cmake)
include(Warnings)
include(Sanitizers)
include(Dependencies)

add_subdirectory(src/common)
add_subdirectory(src/codec)
add_subdirectory(src/crypto)
add_subdirectory(src/storage)
add_subdirectory(src/merkle)
add_subdirectory(src/tx)
add_subdirectory(src/state)
add_subdirectory(src/chain)
add_subdirectory(src/wallet)
add_subdirectory(src/dev)
add_subdirectory(src/cli)

if(YAP_BUILD_TESTS OR YAP_FUZZ)
  enable_testing()
  add_subdirectory(tests)
endif()
