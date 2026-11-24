# SPDX-License-Identifier: Apache-2.0
# ASan + UBSan for all code compiled by this project (libsodium is built separately
# by ExternalProject and is not instrumented).
if(YAP_SANITIZE)
  set(YAP_SANITIZER_FLAGS
    -fsanitize=address,undefined
    -fno-sanitize-recover=all
    -fno-omit-frame-pointer)
  add_compile_options(${YAP_SANITIZER_FLAGS})
  add_link_options(${YAP_SANITIZER_FLAGS})
endif()

# ThreadSanitizer (exclusive with ASan): for the multi-threaded ABCI transport and adapter.
option(YAP_TSAN "Build with ThreadSanitizer" OFF)
if(YAP_TSAN)
  if(YAP_SANITIZE)
    message(FATAL_ERROR "YAP_TSAN and YAP_SANITIZE are mutually exclusive")
  endif()
  add_compile_options(-fsanitize=thread -fno-omit-frame-pointer)
  add_link_options(-fsanitize=thread)
endif()
