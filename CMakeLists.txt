cmake_minimum_required(VERSION 3.25)

project(
  simdfix
  VERSION 0.1.0
  DESCRIPTION "Zero-copy FIX 4.2 tagvalue parser with AVX2 kernels"
  HOMEPAGE_URL "https://github.com/akshat/simdfix"
  LANGUAGES CXX)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

# ---------------------------------------------------------------------------
# Options
# ---------------------------------------------------------------------------
option(SIMDFIX_BUILD_TESTS "Build the Catch2 test suite" ${PROJECT_IS_TOP_LEVEL})
option(SIMDFIX_BUILD_BENCH "Build the Google Benchmark suite" OFF)
option(SIMDFIX_BUILD_TOOLS "Build the corpus generator and other CLI tools" ${PROJECT_IS_TOP_LEVEL})
option(SIMDFIX_BUILD_FUZZERS "Build libFuzzer targets (requires Clang)" OFF)
option(SIMDFIX_WERROR "Treat warnings as errors in simdfix's own targets" ${PROJECT_IS_TOP_LEVEL})
option(SIMDFIX_NATIVE "Compile with -march=native (benchmark builds only, never ship this)" OFF)
option(SIMDFIX_BENCH_HFFIX "Pull in hffix as an external benchmark baseline" ON)

set(SIMDFIX_SANITIZE "" CACHE STRING "Sanitizers to enable, e.g. 'address;undefined'")

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(PROJECT_IS_TOP_LEVEL)
  set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
  if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "" FORCE)
  endif()
endif()

include(CompilerWarnings)
include(Sanitizers)

# ---------------------------------------------------------------------------
# The library: header-only, INTERFACE target.
# ---------------------------------------------------------------------------
add_library(simdfix INTERFACE)
add_library(simdfix::simdfix ALIAS simdfix)

target_include_directories(
  simdfix INTERFACE $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
                    $<INSTALL_INTERFACE:include>)
target_compile_features(simdfix INTERFACE cxx_std_23)

# std::expected is the error-handling story (see include/simdfix/error.hpp).
# Fail at configure time with a readable message rather than 200 lines of
# template spew at compile time.
include(CheckCXXSourceCompiles)
set(CMAKE_REQUIRED_FLAGS "-std=c++23")
check_cxx_source_compiles(
  "#include <expected>
   int main() { std::expected<int, char> e{1}; return *e - 1; }"
  SIMDFIX_HAVE_STD_EXPECTED)
unset(CMAKE_REQUIRED_FLAGS)

if(NOT SIMDFIX_HAVE_STD_EXPECTED)
  set(_simdfix_expected_hint "")
  if(CMAKE_CXX_COMPILER_ID MATCHES "Clang" AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 19)
    # Specific, and easy to lose an afternoon to: libstdc++ gates <expected> on
    # __cpp_concepts >= 202002L, but Clang only started defining that in 19.
    # So Clang 18 + libstdc++ has no std::expected at any -std= level, however
    # new the libstdc++ is. Fixes: use Clang >= 19, use GCC, or use libc++.
    set(_simdfix_expected_hint
        "\n  Clang ${CMAKE_CXX_COMPILER_VERSION} defines __cpp_concepts=201907L, but libstdc++"
        "\n  gates <expected> on __cpp_concepts >= 202002L (Clang 19+). Use clang++-19 or newer,"
        "\n  use GCC, or build against libc++ (-stdlib=libc++ with libc++-dev installed).")
  endif()
  message(
    FATAL_ERROR
      "simdfix requires <expected> (C++23). Need GCC >= 12 or Clang >= 19 with libstdc++.\n"
      "  Found ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}.${_simdfix_expected_hint}")
endif()

# `-march=native` is a benchmark-only convenience. Shipping a binary built this
# way is how you get SIGILL on a customer's older CPU; simdfix's answer is
# runtime dispatch (include/simdfix/detail/cpu.hpp), so the default build stays
# baseline-x86-64 and picks the AVX2 path at run time.
if(SIMDFIX_NATIVE AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  # BUILD_INTERFACE only: never leak a machine-specific flag to an installed
  # package's consumers.
  target_compile_options(simdfix INTERFACE $<BUILD_INTERFACE:-march=native>)
  message(STATUS "simdfix: -march=native enabled (do not distribute these binaries)")
endif()

# Make compile_commands.json visible at the repo root so clangd finds it with
# zero editor configuration.
if(PROJECT_IS_TOP_LEVEL AND CMAKE_EXPORT_COMPILE_COMMANDS AND UNIX)
  file(CREATE_LINK "${CMAKE_BINARY_DIR}/compile_commands.json"
       "${CMAKE_CURRENT_SOURCE_DIR}/compile_commands.json" SYMBOLIC)
endif()

# ---------------------------------------------------------------------------
# Test/bench corpus generator, shared by tools, tests and benchmarks.
# ---------------------------------------------------------------------------
add_library(simdfix_corpus INTERFACE)
add_library(simdfix::corpus ALIAS simdfix_corpus)
target_include_directories(simdfix_corpus INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/tools/include")
target_link_libraries(simdfix_corpus INTERFACE simdfix::simdfix)

# ---------------------------------------------------------------------------
# Subdirectories
# ---------------------------------------------------------------------------
include(FetchContent)
set(FETCHCONTENT_QUIET OFF)

if(SIMDFIX_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

if(SIMDFIX_BUILD_BENCH)
  add_subdirectory(bench)
endif()

if(SIMDFIX_BUILD_TOOLS)
  add_subdirectory(tools)
endif()

if(SIMDFIX_BUILD_FUZZERS)
  add_subdirectory(fuzz)
endif()

# ---------------------------------------------------------------------------
# Install (header-only: just the headers plus a config package)
# ---------------------------------------------------------------------------
if(PROJECT_IS_TOP_LEVEL)
  include(GNUInstallDirs)
  include(CMakePackageConfigHelpers)

  install(DIRECTORY include/ DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")
  install(TARGETS simdfix EXPORT simdfixTargets)
  install(
    EXPORT simdfixTargets
    NAMESPACE simdfix::
    DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/simdfix")

  write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/simdfixConfigVersion.cmake"
    COMPATIBILITY SameMajorVersion
    ARCH_INDEPENDENT)
  install(FILES "${CMAKE_CURRENT_BINARY_DIR}/simdfixConfigVersion.cmake"
          DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/simdfix")
endif()

message(STATUS "simdfix ${PROJECT_VERSION}: tests=${SIMDFIX_BUILD_TESTS} "
               "bench=${SIMDFIX_BUILD_BENCH} fuzz=${SIMDFIX_BUILD_FUZZERS} "
               "native=${SIMDFIX_NATIVE} sanitize='${SIMDFIX_SANITIZE}'")
