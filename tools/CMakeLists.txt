add_executable(gen_corpus gen_corpus.cpp)
target_link_libraries(
  gen_corpus PRIVATE simdfix::simdfix simdfix::corpus simdfix::warnings simdfix::sanitizers)
set_target_properties(gen_corpus PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
