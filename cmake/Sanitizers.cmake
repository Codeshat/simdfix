# Sanitizer wiring, exposed as an INTERFACE target so it lands on both the
# compile and the link line (forgetting the link line is the classic way to get
# "undefined reference to __asan_..." ).
#
# Driven by SIMDFIX_SANITIZE, a semicolon/comma list: address, undefined, thread.
# The `debug-san` preset sets it to "address;undefined".

add_library(simdfix_sanitizers INTERFACE)
add_library(simdfix::sanitizers ALIAS simdfix_sanitizers)

if(SIMDFIX_SANITIZE AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  string(REPLACE "," ";" _simdfix_san_list "${SIMDFIX_SANITIZE}")
  list(REMOVE_DUPLICATES _simdfix_san_list)

  if("thread" IN_LIST _simdfix_san_list AND "address" IN_LIST _simdfix_san_list)
    message(FATAL_ERROR "SIMDFIX_SANITIZE: 'thread' and 'address' are mutually exclusive.")
  endif()

  list(JOIN _simdfix_san_list "," _simdfix_san_flag)
  message(STATUS "simdfix: sanitizers enabled -> ${_simdfix_san_flag}")

  set(_simdfix_san_opts
      -fsanitize=${_simdfix_san_flag}
      -fno-omit-frame-pointer
      -fno-optimize-sibling-calls)

  # Turn UBSan reports into hard failures. Without this, UB is logged and the
  # test still passes green, which is worse than not running UBSan at all.
  if("undefined" IN_LIST _simdfix_san_list)
    list(APPEND _simdfix_san_opts -fno-sanitize-recover=undefined)
  endif()

  target_compile_options(simdfix_sanitizers INTERFACE ${_simdfix_san_opts})
  target_link_options(simdfix_sanitizers INTERFACE -fsanitize=${_simdfix_san_flag})
endif()
