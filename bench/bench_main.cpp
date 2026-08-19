// SPDX-License-Identifier: MIT
//
// Benchmark entry point.
//
// A hand-written main instead of benchmark_main so every run records which
// kernel it used. A pasted throughput number that does not say "avx2" or
// "scalar" next to it is not a result, it is an anecdote.

#include <benchmark/benchmark.h>

#include "simdfix/simdfix.hpp"

int main(int argc, char** argv) {
  benchmark::Initialize(&argc, argv);
  if (benchmark::ReportUnrecognizedArguments(argc, argv)) {
    return 1;
  }

  benchmark::AddCustomContext("simdfix_version", simdfix::version());
  benchmark::AddCustomContext("simdfix_isa", simdfix::active_isa_name());
  benchmark::AddCustomContext("simdfix_note",
                              "set SIMDFIX_DISABLE_AVX2=1 to force the scalar path");

  benchmark::RunSpecifiedBenchmarks();
  benchmark::Shutdown();
  return 0;
}
