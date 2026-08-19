// SPDX-License-Identifier: MIT
//
// Kernel 2 of 2: delimiter scanning.
//
// Parsing tagvalue FIX is, structurally, nothing but "find the next `=`, find
// the next <SOH>, repeat". Everything else is bookkeeping. So the scan is where
// the parse time actually goes, and it is the second kernel.
//
// A note on baselines, because it decides whether the benchmark means anything:
// `memchr` in glibc is already hand-written AVX2. Beating a naive byte loop
// proves nothing. `find_byte_libc` is therefore the baseline that counts, and
// the honest way for a hand-rolled kernel to win is not to be a faster
// `memchr` -- it is to amortise the per-call setup that `memchr` pays on every
// short field, and to extract *several* delimiter positions from one 32-byte
// block instead of restarting the scan at each one.
#ifndef SIMDFIX_SCAN_HPP
#define SIMDFIX_SCAN_HPP

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>

#include "simdfix/detail/cpu.hpp"

#if SIMDFIX_HAS_AVX2
#include <immintrin.h>
#endif

namespace simdfix::kernels {

/// Returned when the byte is not present.
inline constexpr std::size_t npos = std::string_view::npos;

/// Naive reference scan. Correct by inspection; used to validate the others and
/// to show what the vector kernels are actually worth.
[[nodiscard]] constexpr std::size_t find_byte_naive(std::string_view haystack,
                                                    char needle) noexcept {
  for (std::size_t i = 0; i < haystack.size(); ++i) {
    if (haystack[i] == needle) {
      return i;
    }
  }
  return npos;
}

/// glibc `memchr`. This is the number to beat, not the naive loop.
[[nodiscard]] inline std::size_t find_byte_libc(std::string_view haystack, char needle) noexcept {
  if (haystack.empty()) {
    return npos;  // memchr(nullptr, c, 0) is UB even though it "obviously" works.
  }
  const void* hit =
      std::memchr(haystack.data(), static_cast<unsigned char>(needle), haystack.size());
  if (hit == nullptr) {
    return npos;
  }
  return static_cast<std::size_t>(static_cast<const char*>(hit) - haystack.data());
}

#if SIMDFIX_HAS_AVX2

/// Delimiter positions within one 32-byte block, as a bitmask: bit `i` is set
/// iff `block[i] == needle`.
///
/// This is the primitive the field iterator wants. One `vpcmpeqb` +
/// `vpmovmskb` yields *every* delimiter in 32 bytes, and the caller pops them
/// one at a time with `tzcnt` instead of restarting a scan at each hit. That
/// amortisation -- not raw bytes-per-cycle -- is where the win over repeated
/// `memchr` calls on short fields comes from.
///
/// Requires 32 readable bytes at `block`; the caller owns that guarantee.
[[nodiscard]] SIMDFIX_TARGET_AVX2 inline std::uint32_t block_mask_avx2(const char* block,
                                                                       char needle) noexcept {
  const __m256i v = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(block));
  const __m256i n = _mm256_set1_epi8(needle);
  return static_cast<std::uint32_t>(_mm256_movemask_epi8(_mm256_cmpeq_epi8(v, n)));
}

/// AVX2 single-delimiter scan.
///
/// Structure:
///
///   * A 128-byte unrolled loop compares four blocks and ORs the *comparison
///     results* before extracting a single mask. That turns four `vpmovmskb` +
///     four branches into one of each on the common no-hit iteration; the
///     per-block masks are only recomputed on the iteration that actually hits,
///     where the cost is paid once.
///   * Then a plain 32-byte loop, then a scalar tail.
///   * The tail is deliberately scalar. The faster alternative is one
///     overlapping final load, but that requires 32 readable bytes of slack
///     past `haystack.size()`, which this API does not and should not promise:
///     callers hand us `string_view`s that may end exactly at a page boundary.
///     Paying ~16 scalar comparisons to keep the memory-safety contract
///     unconditional is the right trade for a parser that reads hostile input.
///   * No hand-written `_mm256_zeroupper`; the compiler inserts it at the
///     boundary of a `target("avx2")` function.
///
/// Note where the hot path lands in practice: the parser passes the *rest of
/// the message* as the haystack (~100-300 bytes) and the delimiter sits ~12
/// bytes in, so the typical call is one 32-byte block and a `tzcnt`. The scalar
/// tail only runs for haystacks shorter than 32 bytes, i.e. the last field.
[[nodiscard]] SIMDFIX_TARGET_AVX2 inline std::size_t find_byte_avx2(std::string_view haystack,
                                                                    char needle) noexcept {
  const char* const base = haystack.data();
  const std::size_t size = haystack.size();
  const __m256i n = _mm256_set1_epi8(needle);

  std::size_t i = 0;

  for (; i + 128 <= size; i += 128) {
    const __m256i c0 =
        _mm256_cmpeq_epi8(_mm256_loadu_si256(reinterpret_cast<const __m256i*>(base + i)), n);
    const __m256i c1 =
        _mm256_cmpeq_epi8(_mm256_loadu_si256(reinterpret_cast<const __m256i*>(base + i + 32)), n);
    const __m256i c2 =
        _mm256_cmpeq_epi8(_mm256_loadu_si256(reinterpret_cast<const __m256i*>(base + i + 64)), n);
    const __m256i c3 =
        _mm256_cmpeq_epi8(_mm256_loadu_si256(reinterpret_cast<const __m256i*>(base + i + 96)), n);
    const __m256i any = _mm256_or_si256(_mm256_or_si256(c0, c1), _mm256_or_si256(c2, c3));
    if (_mm256_movemask_epi8(any) == 0) {
      continue;
    }
    // Something in these 128 bytes matched; find the first block that did.
    const std::array<std::uint32_t, 4> masks = {
        static_cast<std::uint32_t>(_mm256_movemask_epi8(c0)),
        static_cast<std::uint32_t>(_mm256_movemask_epi8(c1)),
        static_cast<std::uint32_t>(_mm256_movemask_epi8(c2)),
        static_cast<std::uint32_t>(_mm256_movemask_epi8(c3)),
    };
    for (std::size_t k = 0; k < masks.size(); ++k) {
      if (masks[k] != 0) {
        return i + (k * 32) + static_cast<std::size_t>(std::countr_zero(masks[k]));
      }
    }
  }

  for (; i + 32 <= size; i += 32) {
    const std::uint32_t mask = block_mask_avx2(base + i, needle);
    if (mask != 0) {
      return i + static_cast<std::size_t>(std::countr_zero(mask));
    }
  }

  for (; i < size; ++i) {
    if (base[i] == needle) {
      return i;
    }
  }
  return npos;
}

#endif  // SIMDFIX_HAS_AVX2

/// Runtime-dispatched scan. This is what the parser calls.
[[nodiscard]] inline std::size_t find_byte(std::string_view haystack, char needle) noexcept {
#if SIMDFIX_HAS_AVX2
  if (SIMDFIX_LIKELY(detail::has_avx2())) {
    return find_byte_avx2(haystack, needle);
  }
#endif
  return find_byte_libc(haystack, needle);
}

}  // namespace simdfix::kernels

#endif  // SIMDFIX_SCAN_HPP
