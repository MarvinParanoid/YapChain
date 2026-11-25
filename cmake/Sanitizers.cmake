# SPDX-License-Identifier: Apache-2.0
# ASan + UBSan for all code compiled by this project (libsodium is built separately
# by ExternalProject and is not instrumented).
# YAP_SANITIZE_UBSAN=OFF builds ASan alone: only for diagnosing sanitizer cost (docs/ci.md); every
# regular configuration keeps ASan+UBSan.
option(YAP_SANITIZE_UBSAN "With YAP_SANITIZE: also UndefinedBehaviorSanitizer" ON)
if(YAP_SANITIZE)
  if(YAP_SANITIZE_UBSAN)
    set(_yap_san address,undefined)
  else()
    set(_yap_san address)
  endif()
  set(YAP_SANITIZER_FLAGS
    -fsanitize=${_yap_san}
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
