FetchContent_Declare(
  Catch2
  GIT_REPOSITORY https://github.com/catchorg/Catch2.git
  GIT_TAG v3.15.3
  GIT_SHALLOW TRUE
  SYSTEM
  EXCLUDE_FROM_ALL)
set(CATCH_INSTALL_DOCS OFF CACHE BOOL "" FORCE)
set(CATCH_INSTALL_EXTRAS OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(Catch2)

add_executable(
  simdfix_tests
  test_field.cpp
  test_checksum.cpp
  test_scan.cpp
  test_message.cpp
  test_parser.cpp
  test_corpus.cpp)

target_link_libraries(
  simdfix_tests
  PRIVATE simdfix::simdfix
          simdfix::corpus
          simdfix::warnings
          simdfix::sanitizers
          Catch2::Catch2WithMain)

# So tests can say #include "support/fix_text.hpp".
target_include_directories(simdfix_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")

set_target_properties(simdfix_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

list(APPEND CMAKE_MODULE_PATH "${catch2_SOURCE_DIR}/extras")
include(Catch)

# One CTest entry per Catch2 test case: a failure names the case, and `ctest -j`
# actually parallelises.
catch_discover_tests(simdfix_tests)
