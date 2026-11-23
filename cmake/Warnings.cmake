# SPDX-License-Identifier: Apache-2.0
# Strict warnings for first-party targets only (third-party code is included as SYSTEM).
function(yap_target_warnings target)
  set(common_warnings
    -Wall -Wextra -Wpedantic
    -Wconversion -Wsign-conversion
    -Wshadow -Wold-style-cast -Wnon-virtual-dtor -Woverloaded-virtual
    -Wcast-align -Wcast-qual -Wdouble-promotion -Wformat=2
    -Wimplicit-fallthrough -Wmissing-declarations -Wundef)
  if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    list(APPEND common_warnings -Wduplicated-cond -Wduplicated-branches -Wlogical-op)
  endif()
  if(YAP_WERROR)
    list(APPEND common_warnings -Werror)
  endif()
  target_compile_options(${target} PRIVATE ${common_warnings})
  # libstdc++ precondition checks in debug builds (bounds checks on operator[] etc.).
  target_compile_definitions(${target} PRIVATE $<$<CONFIG:Debug>:_GLIBCXX_ASSERTIONS>)
endfunction()
