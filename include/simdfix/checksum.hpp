// SPDX-License-Identifier: MIT
//
// Kernel 1 of 2: the FIX CheckSum.
//
// FIX 4.2 defines CheckSum as the sum of every byte of the message from the
// first byte of `8=` up to and including the <SOH> that terminates the field
// before `10=`, taken modulo 256 and rendered as three ASCII digits.
//
// It is a pure reduction over the whole message with no data dependence, which
// makes it the cleanest possible SIMD target -- and the reason it is kernel 1.
#ifndef SIMDFIX_CHECKSUM_HPP
#define SIMDFIX_CHECKSUM_HPP

#include <cstddef>
#include <cstdint>
#include <string_view>

#include "simdfix/detail/cpu.hpp"

#if SIMDFIX_HAS_AVX2
#include <immintrin.h>
#endif

namespace simdfix::kernels {

/// Reference implementation. `constexpr` so tests can assert against it at
/// compile time, and so it stays the definition of correctness that the vector
/// kernel is checked against.
[[nodiscard]] constexpr std::uint8_t checksum_scalar(std::string_view msg) noexcept {
  std::uint32_t sum = 0;
  for (const char c : msg) {
    sum += static_cast<unsigned char>(c);
  }
  return static_cast<std::uint8_t>(sum & 0xFFU);
}

#if SIMDFIX_HAS_AVX2

/// AVX2 checksum.
///
/// The whole kernel rests on one instruction. `_mm256_sad_epu8(v, zero)`
/// (`vpsadbw`) computes the sum of absolute differences of eight-byte groups;
/// against zero that is just "sum of eight unsigned bytes", widened to a 64-bit
/// lane for free. No widening shuffles, no saturation, no unpacking: one
/// instruction reduces 32 bytes to four partial sums.
///
/// Everything else follows from that:
///
///   * A 64-bit lane cannot overflow until ~2^57 bytes have been summed, so the
///     accumulators never need a periodic flush.
///   * `vpsadbw` has ~3-5 cycle latency and 1/cycle throughput, so a single
///     accumulator would be latency-bound. Four independent accumulators over a
///     128-byte main loop keep the port busy.
///   * The modulo is free. Summing in wide lanes and masking to 8 bits at the
///     very end is exactly congruent to reducing mod 256 at every step, so the
///     low 32 bits of the total are all this function ever needs -- which is
///     also why truncating the horizontal sum to `int` below is not a bug.
///   * The sub-32-byte tail runs scalar. A masked load would avoid the branchy
///     tail, but FIX messages are ~100-300 bytes: the tail is a small constant
///     fraction, and `vpmaskmovd` plus the mask setup costs more than the ~16
///     scalar adds it replaces.
///
/// `test_checksum.cpp` asserts this agrees with `checksum_scalar` at every
/// length from 0 to 4 KiB and at every start alignment, and `fuzz_kernels`
/// checks the same property differentially.
[[nodiscard]] SIMDFIX_TARGET_AVX2 inline std::uint8_t checksum_avx2(std::string_view msg) noexcept {
  const char* p = msg.data();
  std::size_t n = msg.size();

  const __m256i zero = _mm256_setzero_si256();
  __m256i acc0 = zero;
  __m256i acc1 = zero;
  __m256i acc2 = zero;
  __m256i acc3 = zero;

  // 128 bytes per iteration, four dependency chains.
  while (n >= 128) {
    const __m256i v0 = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(p));
    const __m256i v1 = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(p + 32));
    const __m256i v2 = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(p + 64));
    const __m256i v3 = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(p + 96));
    acc0 = _mm256_add_epi64(acc0, _mm256_sad_epu8(v0, zero));
    acc1 = _mm256_add_epi64(acc1, _mm256_sad_epu8(v1, zero));
    acc2 = _mm256_add_epi64(acc2, _mm256_sad_epu8(v2, zero));
    acc3 = _mm256_add_epi64(acc3, _mm256_sad_epu8(v3, zero));
    p += 128;
    n -= 128;
  }

  while (n >= 32) {
    const __m256i v = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(p));
    acc0 = _mm256_add_epi64(acc0, _mm256_sad_epu8(v, zero));
    p += 32;
    n -= 32;
  }

  acc0 = _mm256_add_epi64(acc0, acc1);
  acc2 = _mm256_add_epi64(acc2, acc3);
  acc0 = _mm256_add_epi64(acc0, acc2);

  // Fold 4x64 -> 1x64, then take the low 32 bits (see the modulo note above).
  const __m128i halves =
      _mm_add_epi64(_mm256_castsi256_si128(acc0), _mm256_extracti128_si256(acc0, 1));
  const __m128i folded = _mm_add_epi64(halves, _mm_unpackhi_epi64(halves, halves));
  auto sum = static_cast<std::uint32_t>(_mm_cvtsi128_si32(folded));

  for (std::size_t i = 0; i < n; ++i) {
    sum += static_cast<unsigned char>(p[i]);
  }
  return static_cast<std::uint8_t>(sum & 0xFFU);
}

#endif  // SIMDFIX_HAS_AVX2

/// Runtime-dispatched checksum. This is what the parser calls.
[[nodiscard]] inline std::uint8_t checksum(std::string_view msg) noexcept {
#if SIMDFIX_HAS_AVX2
  if (SIMDFIX_LIKELY(detail::has_avx2())) {
    return checksum_avx2(msg);
  }
#endif
  return checksum_scalar(msg);
}

/// Render a checksum as the three zero-padded ASCII digits FIX puts in tag 10.
/// Writes exactly 3 bytes to `out`.
inline void format_checksum(std::uint8_t sum, char* out) noexcept {
  out[0] = static_cast<char>('0' + (sum / 100));
  out[1] = static_cast<char>('0' + ((sum / 10) % 10));
  out[2] = static_cast<char>('0' + (sum % 10));
}

/// Parse the three-digit body of tag 10. Rejects anything that is not exactly
/// three ASCII digits -- FIX 4.2 fixes the width, so `10=7<SOH>` is malformed
/// rather than "7".
[[nodiscard]] constexpr bool parse_checksum(std::string_view digits, std::uint8_t& out) noexcept {
  if (digits.size() != 3) {
    return false;
  }
  std::uint32_t value = 0;
  for (const char c : digits) {
    const auto d = static_cast<unsigned>(static_cast<unsigned char>(c)) - unsigned{'0'};
    if (d > 9U) {
      return false;
    }
    value = (value * 10) + d;
  }
  // 999 fits in the three-digit field but not in a byte; a checksum can only
  // ever be 000-255, so anything above is malformed.
  if (value > 255U) {
    return false;
  }
  out = static_cast<std::uint8_t>(value);
  return true;
}

}  // namespace simdfix::kernels

#endif  // SIMDFIX_CHECKSUM_HPP
