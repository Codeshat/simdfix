// SPDX-License-Identifier: MIT
//
// Differential fuzzing of the SIMD kernels against their scalar references.
//
// This is the highest-value test in the repo. A
// unit test checks the lengths and alignments its author thought of; this
// checks the ones they did not. Any divergence between a vector kernel and its
// reference is, by construction, a bug in the vector kernel.
//
//   ./build/fuzz/bin/fuzz_kernels -max_total_time=120

#include <cstddef>
#include <cstdint>
#include <string_view>

#include "simdfix/checksum.hpp"
#include "simdfix/detail/cpu.hpp"
#include "simdfix/scan.hpp"

namespace {

void abort_if(bool condition) {
  if (condition) {
    __builtin_trap();
  }
}

}  // namespace

// NOLINTNEXTLINE(readability-identifier-naming): the name is libFuzzer's ABI.
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  if (size == 0) {
    return 0;
  }

  // First byte picks the needle, so the fuzzer can steer toward delimiters that
  // are dense, absent, or land exactly on a 32-byte boundary.
  const char needle = static_cast<char>(data[0]);
  const std::string_view buffer{reinterpret_cast<const char*>(data) + 1, size - 1};

  // Scanning: all implementations must agree, including on "not found".
  const std::size_t naive = simdfix::kernels::find_byte_naive(buffer, needle);
  abort_if(simdfix::kernels::find_byte_libc(buffer, needle) != naive);
  abort_if(simdfix::kernels::find_byte(buffer, needle) != naive);

  // Checksum: the reference is the definition.
  const std::uint8_t expected = simdfix::kernels::checksum_scalar(buffer);
  abort_if(simdfix::kernels::checksum(buffer) != expected);

#if SIMDFIX_HAS_AVX2
  if (simdfix::detail::has_avx2()) {
    abort_if(simdfix::kernels::find_byte_avx2(buffer, needle) != naive);
    abort_if(simdfix::kernels::checksum_avx2(buffer) != expected);

    if (buffer.size() >= 32) {
      // Exercise the block primitive at every start offset the buffer allows,
      // which is where a lane-order or sign-extension mistake surfaces.
      for (std::size_t off = 0; off + 32 <= buffer.size(); off += 7) {
        std::uint32_t reference = 0;
        for (std::uint32_t i = 0; i < 32; ++i) {
          if (buffer[off + i] == needle) {
            reference |= (std::uint32_t{1} << i);
          }
        }
        abort_if(simdfix::kernels::block_mask_avx2(buffer.data() + off, needle) != reference);
      }
    }
  }
#endif

  // Every suffix must checksum consistently with the prefix sums, which catches
  // tail-handling bugs the fixed-length tests miss.
  for (std::size_t drop = 1; drop < 40 && drop <= buffer.size(); ++drop) {
    const std::string_view tail = buffer.substr(drop);
    abort_if(simdfix::kernels::checksum(tail) != simdfix::kernels::checksum_scalar(tail));
  }

  return 0;
}
