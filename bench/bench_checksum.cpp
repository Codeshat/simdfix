// SPDX-License-Identifier: MIT
//
// Kernel 1 throughput: scalar vs AVX2.
//
// Two axes are measured, because they answer different questions:
//   * `by_size` sweeps buffer length. It shows where the vector kernel starts
//     to pay for itself, which for a 32-byte-block reduction is somewhere just
//     above 32-64 bytes. Reporting only the 4 KiB number would hide that a
//     150-byte FIX message may never reach the asymptote.
//   * `corpus` runs the real message-length distribution. That is the number
//     that belongs in the README.
//
// The kernel is selected with a template parameter rather than a function
// pointer on purpose: a function-pointer call cannot be inlined, which at 16-
// and 32-byte buffers is a large fraction of the measurement. Note that the
// AVX2 kernel still pays a real call, because a `target("avx2")` function
// cannot be inlined into a baseline caller -- that cost is inherent to runtime
// dispatch and belongs in the number.

#include <cstddef>
#include <cstdint>
#include <random>
#include <string>
#include <string_view>

#include <benchmark/benchmark.h>

#include "bench_support.hpp"
#include "simdfix/checksum.hpp"
#include "simdfix/detail/cpu.hpp"
#include "simdfix/message.hpp"

namespace {

enum class impl { scalar, avx2, dispatched };

template<impl I>
[[nodiscard]] SIMDFIX_ALWAYS_INLINE std::uint8_t run(std::string_view s) noexcept {
  if constexpr (I == impl::scalar) {
    return simdfix::kernels::checksum_scalar(s);
  } else if constexpr (I == impl::dispatched) {
    return simdfix::kernels::checksum(s);
  } else {
#if SIMDFIX_HAS_AVX2
    return simdfix::kernels::checksum_avx2(s);
#else
    return simdfix::kernels::checksum_scalar(s);
#endif
  }
}

const std::string& random_buffer() {
  static const std::string data = [] {
    std::mt19937_64 rng{0x5EEDULL};
    std::string s(1U << 20U, '\0');
    for (char& c : s) {
      c = static_cast<char>(rng() & 0xFFU);
    }
    return s;
  }();
  return data;
}

template<impl I>
void checksum_by_size(benchmark::State& state) {
  const auto len = static_cast<std::size_t>(state.range(0));
  const std::string_view buf{random_buffer().data(), len};

  for (auto _ : state) {
    simdfix::bench::sink(run<I>(buf));
  }

  state.SetBytesProcessed(state.iterations() * static_cast<std::int64_t>(len));
  state.SetLabel(simdfix::detail::to_string(simdfix::detail::active_isa()));
}

template<impl I>
void checksum_corpus(benchmark::State& state) {
  const auto& messages = simdfix::bench::corpus_messages();
  std::uint32_t mixed = 0;

  for (auto _ : state) {
    for (const std::string_view msg : messages) {
      // The checksum covers everything but the 7-byte trailer.
      mixed += run<I>(msg.substr(0, msg.size() - simdfix::trailer_size));
    }
    simdfix::bench::sink(mixed);
  }

  const auto bytes = static_cast<std::int64_t>(simdfix::bench::corpus_bytes().size());
  state.SetBytesProcessed(state.iterations() * bytes);
  state.SetItemsProcessed(state.iterations() * static_cast<std::int64_t>(messages.size()));
}

}  // namespace

BENCHMARK(checksum_by_size<impl::scalar>)
    ->Name("checksum/scalar")
    ->RangeMultiplier(2)
    ->Range(16, 4096);

#if SIMDFIX_HAS_AVX2
BENCHMARK(checksum_by_size<impl::avx2>)->Name("checksum/avx2")->RangeMultiplier(2)->Range(16, 4096);
#endif

BENCHMARK(checksum_by_size<impl::dispatched>)
    ->Name("checksum/dispatched")
    ->RangeMultiplier(2)
    ->Range(16, 4096);

BENCHMARK(checksum_corpus<impl::scalar>)->Name("checksum/corpus/scalar");

#if SIMDFIX_HAS_AVX2
BENCHMARK(checksum_corpus<impl::avx2>)->Name("checksum/corpus/avx2");
#endif
