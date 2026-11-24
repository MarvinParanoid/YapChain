# SPDX-License-Identifier: Apache-2.0
#
# Pinned protobuf + abseil for the ABCI bindings (ADR-0027). Built once per build directory at
# configure time from SHA-256-verified release archives (Release, the project's own compilers),
# installed into a private prefix and imported with find_package(... NO_DEFAULT_PATH). System copies
# are never used.
#
# Sanitizer builds instrument the dependencies too (ASan for YAP_SANITIZE / YAP_FUZZ, TSan for
# YAP_TSAN): protobuf's RepeatedField carries ASan container annotations in its headers, and an
# uninstrumented libprotobuf growing the same buffers produces false "container-overflow" reports;
# TSan likewise needs every thread-touching library instrumented. UBSan is not applied to the
# dependencies — only the project's own code is held to -fno-sanitize-recover.

set(YAP_ABSEIL_VERSION 20260817.0)
set(YAP_ABSEIL_SHA256 f7e05179df39c45434cad433f5783840bb3788ef322976f9138bc6b72b3a107d)
set(YAP_PROTOBUF_VERSION 36.2)
set(YAP_PROTOBUF_SHA256 3d9642a662d10e68ebae5e53f14dcce5105684212d5078f8e0d47d1ab3ae6b64)

set(_yap_pb_root "${CMAKE_BINARY_DIR}/_deps/protobuf-superbuild")
set(_yap_pb_prefix "${_yap_pb_root}/install")
set(_yap_pb_san "")
if(YAP_SANITIZE OR YAP_FUZZ)
  set(_yap_pb_san "-fsanitize=address -fno-omit-frame-pointer")
  set(_yap_pb_san_tag "asan")
elseif(YAP_TSAN)
  set(_yap_pb_san "-fsanitize=thread -fno-omit-frame-pointer")
  set(_yap_pb_san_tag "tsan")
else()
  set(_yap_pb_san_tag "plain")
endif()
set(_yap_pb_stamp "${_yap_pb_prefix}/.yap-built-absl-${YAP_ABSEIL_VERSION}-pb-${YAP_PROTOBUF_VERSION}-nozlib-${_yap_pb_san_tag}")
# An instrumented protoc runs at build time; its exit-time allocations are not ours to police.
set(_yap_pb_env ASAN_OPTIONS=detect_leaks=0)

function(_yap_fetch_archive name file url sha256 out_dir)
  set(_archive "${_yap_pb_root}/${file}")
  if(YAP_DEPS_DIR)
    file(COPY_FILE "${YAP_DEPS_DIR}/${file}" "${_archive}" ONLY_IF_DIFFERENT)
  elseif(NOT EXISTS "${_archive}")
    file(DOWNLOAD "${url}" "${_archive}" STATUS _st TLS_VERIFY ON)
    list(GET _st 0 _code)
    if(NOT _code EQUAL 0)
      message(FATAL_ERROR "download of ${name} failed: ${_st}")
    endif()
  endif()
  file(SHA256 "${_archive}" _actual)
  if(NOT _actual STREQUAL sha256)
    file(REMOVE "${_archive}")
    message(FATAL_ERROR "${name}: SHA-256 mismatch (expected ${sha256}, got ${_actual})")
  endif()
  file(REMOVE_RECURSE "${_yap_pb_root}/src-${name}")
  file(ARCHIVE_EXTRACT INPUT "${_archive}" DESTINATION "${_yap_pb_root}/src-${name}")
  file(GLOB _top LIST_DIRECTORIES true "${_yap_pb_root}/src-${name}/*")
  set(${out_dir} "${_top}" PARENT_SCOPE)
endfunction()

function(_yap_build name src_dir)
  set(_bld "${_yap_pb_root}/build-${name}")
  file(REMOVE_RECURSE "${_bld}")
  cmake_host_system_information(RESULT _jobs QUERY NUMBER_OF_LOGICAL_CORES)
  execute_process(
    COMMAND "${CMAKE_COMMAND}" -S "${src_dir}" -B "${_bld}" -G Ninja
            -DCMAKE_BUILD_TYPE=Release
            "-DCMAKE_C_FLAGS=${_yap_pb_san}" "-DCMAKE_CXX_FLAGS=${_yap_pb_san}"
            "-DCMAKE_EXE_LINKER_FLAGS=${_yap_pb_san}" "-DCMAKE_SHARED_LINKER_FLAGS=${_yap_pb_san}"
            -DCMAKE_C_COMPILER=${CMAKE_C_COMPILER} -DCMAKE_CXX_COMPILER=${CMAKE_CXX_COMPILER}
            -DCMAKE_CXX_STANDARD=20 -DCMAKE_POSITION_INDEPENDENT_CODE=ON
            -DCMAKE_INSTALL_PREFIX=${_yap_pb_prefix} -DCMAKE_PREFIX_PATH=${_yap_pb_prefix}
            -DCMAKE_INSTALL_LIBDIR=lib
            ${ARGN}
    RESULT_VARIABLE _rc OUTPUT_FILE "${_yap_pb_root}/${name}-configure.log" ERROR_FILE "${_yap_pb_root}/${name}-configure.log")
  if(NOT _rc EQUAL 0)
    message(FATAL_ERROR "configuring ${name} failed, see ${_yap_pb_root}/${name}-configure.log")
  endif()
  execute_process(COMMAND "${CMAKE_COMMAND}" -E env ${_yap_pb_env} "${CMAKE_COMMAND}" --build "${_bld}" --parallel ${_jobs} --target install
    RESULT_VARIABLE _rc OUTPUT_FILE "${_yap_pb_root}/${name}-build.log" ERROR_FILE "${_yap_pb_root}/${name}-build.log")
  if(NOT _rc EQUAL 0)
    message(FATAL_ERROR "building ${name} failed, see ${_yap_pb_root}/${name}-build.log")
  endif()
  file(REMOVE_RECURSE "${_bld}")
endfunction()

if(NOT EXISTS "${_yap_pb_stamp}")
  file(REMOVE_RECURSE "${_yap_pb_prefix}")
  message(STATUS "Building pinned abseil ${YAP_ABSEIL_VERSION} and protobuf ${YAP_PROTOBUF_VERSION} (once per build dir)...")
  file(MAKE_DIRECTORY "${_yap_pb_root}")
  _yap_fetch_archive(abseil "abseil-cpp-${YAP_ABSEIL_VERSION}.tar.gz"
    "https://github.com/abseil/abseil-cpp/releases/download/${YAP_ABSEIL_VERSION}/abseil-cpp-${YAP_ABSEIL_VERSION}.tar.gz"
    ${YAP_ABSEIL_SHA256} _absl_src)
  _yap_build(abseil "${_absl_src}" -DABSL_PROPAGATE_CXX_STD=ON -DABSL_BUILD_TESTING=OFF -DABSL_ENABLE_INSTALL=ON)
  _yap_fetch_archive(protobuf "protobuf-${YAP_PROTOBUF_VERSION}.tar.gz"
    "https://github.com/protocolbuffers/protobuf/releases/download/v${YAP_PROTOBUF_VERSION}/protobuf-${YAP_PROTOBUF_VERSION}.tar.gz"
    ${YAP_PROTOBUF_SHA256} _pb_src)
  _yap_build(protobuf "${_pb_src}" -Dprotobuf_BUILD_TESTS=OFF -Dprotobuf_ABSL_PROVIDER=package
    -Dprotobuf_BUILD_SHARED_LIBS=OFF -Dprotobuf_INSTALL=ON -Dprotobuf_BUILD_LIBUPB=OFF
    -Dprotobuf_WITH_ZLIB=OFF)  # no system zlib (ADR-0018); ABCI does not need gzip streams
  file(REMOVE_RECURSE "${_yap_pb_root}/src-abseil" "${_yap_pb_root}/src-protobuf")
  file(TOUCH "${_yap_pb_stamp}")
endif()

find_package(absl CONFIG REQUIRED PATHS "${_yap_pb_prefix}" NO_DEFAULT_PATH)
find_package(utf8_range CONFIG REQUIRED PATHS "${_yap_pb_prefix}" NO_DEFAULT_PATH)
find_package(protobuf CONFIG REQUIRED PATHS "${_yap_pb_prefix}" NO_DEFAULT_PATH)
if(NOT protobuf_VERSION VERSION_EQUAL "6.${YAP_PROTOBUF_VERSION}" AND NOT protobuf_VERSION VERSION_EQUAL "${YAP_PROTOBUF_VERSION}")
  message(STATUS "protobuf package version ${protobuf_VERSION} (release ${YAP_PROTOBUF_VERSION})")
endif()

# Generated C++ bindings for the vendored CometBFT v0.40.0 schemas.
set(_yap_proto_root "${PROJECT_SOURCE_DIR}/third_party/cometbft-proto")
set(_yap_proto_files
  gogoproto/gogo.proto
  tendermint/crypto/keys.proto
  tendermint/crypto/proof.proto
  tendermint/types/params.proto
  tendermint/types/validator.proto
  tendermint/abci/types.proto)
set(_yap_gen "${CMAKE_BINARY_DIR}/generated/abci-proto")
set(_yap_gen_srcs "")
set(_yap_proto_abs "")
foreach(f IN LISTS _yap_proto_files)
  string(REGEX REPLACE "\\.proto$" "" _base "${f}")
  list(APPEND _yap_gen_srcs "${_yap_gen}/${_base}.pb.cc")
  list(APPEND _yap_proto_abs "${_yap_proto_root}/${f}")
endforeach()
add_custom_command(
  OUTPUT ${_yap_gen_srcs}
  COMMAND "${CMAKE_COMMAND}" -E make_directory "${_yap_gen}"
  COMMAND "${CMAKE_COMMAND}" -E env ${_yap_pb_env} $<TARGET_FILE:protobuf::protoc> --cpp_out=${_yap_gen} -I${_yap_proto_root} -I${_yap_pb_prefix}/include ${_yap_proto_files}
  DEPENDS ${_yap_proto_abs} protobuf::protoc
  WORKING_DIRECTORY "${_yap_proto_root}"
  COMMENT "Generating ABCI protobuf bindings (CometBFT v0.40.0)")
add_library(yap_abci_proto STATIC ${_yap_gen_srcs})
target_include_directories(yap_abci_proto SYSTEM PUBLIC "${_yap_gen}")
target_link_libraries(yap_abci_proto PUBLIC protobuf::libprotobuf)
target_compile_options(yap_abci_proto PRIVATE -w)
add_library(yap::abci_proto ALIAS yap_abci_proto)
