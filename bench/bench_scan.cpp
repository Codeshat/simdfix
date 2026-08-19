// SPDX-License-Identifier: MIT
//
// Kernel 2 throughput: delimiter scanning.
//
// `long_scan` is the flattering benchmark -- one call over 4 KiB, where any
// competent vector kernel looks good. `field_walk` is the honest one: it
// reproduces what the parser actually does, which is a great many *short*
// scans, one per field, averaging ~12 bytes. Per-call setup dominates there,
// and that is exactly why `memchr` is beatable in this workload despite being
// hand-tuned AVX2 itself.
//
// If a kernel wins `long_scan` but not `field_walk`, it has not improved the
// parser.

#include <cstddef>
#include <cstdint>
#include <random>
#include <string>
#include <string_view>

#include <benchmark/benchmark.h>

#include "bench_support.hpp"
#include "simdfix/detail/cpu.hpp"
#include "simdfix/field.hpp"
#include "simdfix/scan.hpp"

namespace {

enum class impl { naive, libc, avx2, dispatched };

template<impl I>
[[nodiscard]] SIMDFIX_ALWAYS_INLINE std::size_t run(std::string_view h, char needle) noexcept {
  if constexpr (I == impl::naive) {
    return simdfix::kernels::find_byte_naive(h, needle);
  } else if constexpr (I == impl::libc) {
    return simdfix::kernels::find_byte_libc(h, needle);
  } else if constexpr (I == impl::dispatched) {
    return simdfix::kernels::find_byte(h, needle);
  } else {
#if SIMDFIX_HAS_AVX2
    return simdfix::kernels::find_byte_avx2(h, needle);
#else
    return simdfix::kernels::find_byte_libc(h, needle);
#endif
  }
}

constexpr char needle = '\x02';

/// A buffer whose only occurrence of `needle` is at the very end, so the scan
/// is forced to traverse the whole length rather than exiting early.
const std::string& long_buffer() {
  static const std::string data = [] {
    std::string s(1U << 20U, 'x');
    s.back() = needle;
    return s;
  }();
  return data;
}

template<impl I>
void long_scan(benchmark::State& state) {
  const auto len = static_cast<std::size_t>(state.range(0));
  const std::string_view buf{long_buffer().data() + long_buffer().size() - len, len};

  for (auto _ : state) {
    simdfix::bench::sink(run<I>(buf, needle));
  }

  state.SetBytesProcessed(state.iterations() * static_cast<std::int64_t>(len));
  state.SetLabel(simdfix::detail::to_string(simdfix::detail::active_isa()));
}

/// Walk every field of every message the way the parser does: find `=`, find
/// SOH, advance. This is the workload that decides whether kernel 2 was worth
/// writing.
template<impl I>
void field_walk(benchmark::State& state) {
  const auto& messages = simdfix::bench::corpus_messages();
  std::size_t fields = 0;

  for (auto _ : state) {
    for (std::string_view msg : messages) {
      while (!msg.empty()) {
        const std::size_t eq = run<I>(msg, simdfix::equals);
        if (eq == simdfix::kernels::npos) {
          break;
        }
        const std::string_view after = msg.substr(eq + 1);
        const std::size_t end = run<I>(after, simdfix::soh);
        if (end == simdfix::kernels::npos) {
          break;
        }
        ++fields;
        msg = after.substr(end + 1);
      }
    }
    simdfix::bench::sink(fields);
  }

  const auto bytes = static_cast<std::int64_t>(simdfix::bench::corpus_bytes().size());
  state.SetBytesProcessed(state.iterations() * bytes);
  state.SetItemsProcessed(static_cast<std::int64_t>(fields));
  state.counters["mean_scan_len"] =
      benchmark::Counter(simdfix::bench::mean_message_size(), benchmark::Counter::kDefaults);
}

}  // namespace

BENCHMARK(long_scan<impl::naive>)->Name("scan/long/naive")->RangeMultiplier(8)->Range(64, 65536);
BENCHMARK(long_scan<impl::libc>)->Name("scan/long/libc")->RangeMultiplier(8)->Range(64, 65536);
#if SIMDFIX_HAS_AVX2
BENCHMARK(long_scan<impl::avx2>)->Name("scan/long/avx2")->RangeMultiplier(8)->Range(64, 65536);
#endif

BENCHMARK(field_walk<impl::naive>)->Name("scan/field_walk/naive");
BENCHMARK(field_walk<impl::libc>)->Name("scan/field_walk/libc");
#if SIMDFIX_HAS_AVX2
BENCHMARK(field_walk<impl::avx2>)->Name("scan/field_walk/avx2");
#endif
BENCHMARK(field_walk<impl::dispatched>)->Name("scan/field_walk/dispatched");
