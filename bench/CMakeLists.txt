FetchContent_Declare(
  benchmark
  GIT_REPOSITORY https://github.com/google/benchmark.git
  GIT_TAG v1.9.5
  GIT_SHALLOW TRUE
  SYSTEM
  EXCLUDE_FROM_ALL)
set(BENCHMARK_ENABLE_TESTING OFF CACHE BOOL "" FORCE)
set(BENCHMARK_ENABLE_GTEST_TESTS OFF CACHE BOOL "" FORCE)
set(BENCHMARK_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
set(BENCHMARK_ENABLE_WERROR OFF CACHE BOOL "" FORCE)
set(BENCHMARK_INSTALL_DOCS OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(benchmark)

set(_simdfix_bench_sources bench_main.cpp bench_checksum.cpp bench_scan.cpp bench_parse.cpp)

# hffix is the external baseline. Without it the benchmark section says "fast
# compared to nothing"; with it, the numbers are checked against a library other
# people already trust.
if(SIMDFIX_BENCH_HFFIX)
  FetchContent_Declare(
    hffix
    GIT_REPOSITORY https://github.com/jamesdbrock/hffix.git
    GIT_TAG v1.4.1
    GIT_SHALLOW TRUE
    # hffix ships a CMakeLists that builds its own tests and docs. Point
    # SOURCE_SUBDIR at a directory that does not exist so FetchContent downloads
    # the sources without configuring the project, then wrap the headers here.
    SOURCE_SUBDIR do-not-configure)
  FetchContent_MakeAvailable(hffix)

  add_library(hffix_headers INTERFACE)
  # SYSTEM: hffix is not compiled under simdfix's -Werror -Wconversion policy.
  target_include_directories(hffix_headers SYSTEM INTERFACE "${hffix_SOURCE_DIR}/include")

  list(APPEND _simdfix_bench_sources bench_hffix.cpp)
endif()

add_executable(simdfix_bench ${_simdfix_bench_sources})

target_link_libraries(
  simdfix_bench PRIVATE simdfix::simdfix simdfix::corpus simdfix::warnings
                        benchmark::benchmark)

if(SIMDFIX_BENCH_HFFIX)
  target_link_libraries(simdfix_bench PRIVATE hffix_headers)
  target_compile_definitions(simdfix_bench PRIVATE SIMDFIX_BENCH_HFFIX=1)
endif()

# Deliberately NOT linked against simdfix::sanitizers: ASan changes memory
# layout and instruments every load, so sanitized timings are meaningless. If a
# benchmark needs sanitizing, it is a test.
set_target_properties(simdfix_bench PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

if(SIMDFIX_SANITIZE)
  message(WARNING "simdfix: benchmarks are built without sanitizers even though "
                  "SIMDFIX_SANITIZE='${SIMDFIX_SANITIZE}'; timings from a sanitized "
                  "build would be meaningless.")
endif()
