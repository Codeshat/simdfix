# A single INTERFACE target carrying simdfix's warning policy.
#
# This is deliberately NOT applied globally: FetchContent pulls in Catch2,
# Google Benchmark and hffix, and compiling third-party code under -Werror
# -Wconversion is a losing game. Only simdfix's own targets link this.

add_library(simdfix_warnings INTERFACE)
add_library(simdfix::warnings ALIAS simdfix_warnings)

set(_simdfix_gcc_clang_warnings
    -Wall
    -Wextra
    -Wpedantic
    -Wconversion
    -Wsign-conversion
    -Wshadow
    -Wcast-qual
    -Wcast-align
    -Wdouble-promotion
    -Wformat=2
    -Wimplicit-fallthrough
    -Wnon-virtual-dtor
    -Wold-style-cast
    -Woverloaded-virtual
    -Wnull-dereference
    -Wunused)

set(_simdfix_gcc_only_warnings
    -Wduplicated-cond
    -Wduplicated-branches
    -Wlogical-op
    -Wuseless-cast)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU")
  list(APPEND _simdfix_gcc_clang_warnings ${_simdfix_gcc_only_warnings})
endif()

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(simdfix_warnings INTERFACE ${_simdfix_gcc_clang_warnings})
  if(SIMDFIX_WERROR)
    target_compile_options(simdfix_warnings INTERFACE -Werror)
  endif()
elseif(MSVC)
  target_compile_options(simdfix_warnings INTERFACE /W4 /permissive-)
  if(SIMDFIX_WERROR)
    target_compile_options(simdfix_warnings INTERFACE /WX)
  endif()
endif()
