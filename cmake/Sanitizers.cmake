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
