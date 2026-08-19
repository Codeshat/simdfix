// SPDX-License-Identifier: MIT
//
// Shared benchmark fixtures.
//
// The corpus is built once per process and shared by every benchmark, so no
// benchmark measures generation cost, and every benchmark measures the same
// bytes. Both matter: a corpus regenerated per iteration would swamp the
// numbers, and a different corpus per benchmark would make the comparisons
// meaningless.
#ifndef SIMDFIX_BENCH_SUPPORT_HPP
#define SIMDFIX_BENCH_SUPPORT_HPP

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include <benchmark/benchmark.h>

#include "simdfix/detail/cpu.hpp"
#include "simdfix_corpus/corpus.hpp"

namespace simdfix::bench {

inline constexpr std::size_t default_message_count = 20'000;

/// Consume a value so the optimiser cannot delete the work that produced it.
///
/// Wraps benchmark::DoNotOptimize because its `const Tp&` overload is deprecated
/// as of Benchmark 1.9 -- a const reference lets the compiler assume the value
/// is never modified and optimise around it, which is the exact failure mode
/// DoNotOptimize exists to prevent. Taking the argument by value here gives a
/// mutable local, so the non-deprecated lvalue overload is always the one
/// selected, and callers can pass const refs and rvalues alike.
template<typename T>
SIMDFIX_ALWAYS_INLINE void sink(T value) noexcept {
  benchmark::DoNotOptimize(value);
}

/// Process-wide corpus, generated exactly once on first use.
[[nodiscard]] inline const std::string& corpus_bytes() {
  static const std::string data = [] {
    corpus::options opts;
    opts.message_count = default_message_count;
    return corpus::generate(opts);
  }();
  return data;
}

/// One view per message in `corpus_bytes()`.
[[nodiscard]] inline const std::vector<std::string_view>& corpus_messages() {
  static const std::vector<std::string_view> messages = corpus::split(corpus_bytes());
  return messages;
}

/// Mean message size, for reporting bytes/second per message.
[[nodiscard]] inline double mean_message_size() {
  const auto& msgs = corpus_messages();
  return msgs.empty()
             ? 0.0
             : static_cast<double>(corpus_bytes().size()) / static_cast<double>(msgs.size());
}

}  // namespace simdfix::bench

#endif  // SIMDFIX_BENCH_SUPPORT_HPP
