if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  message(FATAL_ERROR "SIMDFIX_BUILD_FUZZERS requires Clang (libFuzzer). Use --preset fuzz.")
endif()

function(simdfix_add_fuzzer name)
  add_executable(${name} ${name}.cpp)
  target_link_libraries(${name} PRIVATE simdfix::simdfix simdfix::corpus simdfix::warnings)
  target_compile_options(${name} PRIVATE -fsanitize=fuzzer)
  target_link_options(${name} PRIVATE -fsanitize=fuzzer)
  # Sanitizers are wanted here, unlike in bench/: a fuzzer without ASan finds
  # only the crashes that happen to be immediately fatal.
  target_link_libraries(${name} PRIVATE simdfix::sanitizers)
  set_target_properties(${name} PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
endfunction()

simdfix_add_fuzzer(fuzz_parse)
simdfix_add_fuzzer(fuzz_kernels)
