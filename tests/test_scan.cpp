// SPDX-License-Identifier: MIT
//
// Kernel 2 equivalence and correctness.
//
// Same contract as the checksum tests: `find_byte_naive` is the definition and
// every other implementation is checked against it. The cases that matter for a
// 32-byte-block scan are the tail (length not a multiple of 32), the start
// alignment, and a needle that appears more than once -- `find_byte` must
// return the *first*, which is what makes the `tzcnt` on the block mask load-
// bearing rather than incidental.

#include <cstddef>
#include <cstdint>
#include <random>
#include <string>
#include <string_view>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "simdfix/detail/cpu.hpp"
#include "simdfix/scan.hpp"

using simdfix::kernels::find_byte;
using simdfix::kernels::find_byte_libc;
using simdfix::kernels::find_byte_naive;
using simdfix::kernels::npos;

namespace {

constexpr char filler = 'x';
constexpr char needle = '\x01';

/// Padded so a subview can begin at any of 64 alignments.
std::vector<char> padded_buffer(std::size_t n) {
  return std::vector<char>(n + 128, filler);
}

}  // namespace

TEST_CASE("find_byte_naive is the reference", "[scan]") {
  CHECK(find_byte_naive("", needle) == npos);
  CHECK(find_byte_naive("abc", needle) == npos);
  CHECK(find_byte_naive("\x01", needle) == 0);
  CHECK(find_byte_naive("ab\x01"
                        "cd",
                        needle) == 2);
  CHECK(find_byte_naive("ab\x01"
                        "cd\x01",
                        needle) == 2);  // first, not last

  static_assert(find_byte_naive("abc", 'c') == 2);
  static_assert(find_byte_naive("abc", 'z') == npos);
}

TEST_CASE("find_byte_libc agrees with the reference", "[scan]") {
  CHECK(find_byte_libc("", needle) == npos);
  CHECK(find_byte_libc("abc", needle) == npos);
  CHECK(find_byte_libc("\x01", needle) == 0);
  CHECK(find_byte_libc("ab\x01"
                       "cd\x01",
                       needle) == 2);

  SECTION("an empty view must not reach memchr with a null pointer") {
    // memchr(nullptr, c, 0) is UB even though every implementation tolerates it;
    // under UBSan this assertion is the one that would fire.
    const std::string_view empty{};
    CHECK(empty.data() == nullptr);
    CHECK(find_byte_libc(empty, needle) == npos);
  }

  SECTION("high-bit needles are compared as unsigned") {
    const std::string buf = std::string("aaa") + '\xFF' + "bbb";
    CHECK(find_byte_libc(buf, '\xFF') == 3);
    CHECK(find_byte_naive(buf, '\xFF') == 3);
  }
}

TEST_CASE("all scan implementations agree exhaustively on small inputs", "[scan]") {
  // Every length up to 96 bytes, needle at every position plus the absent case,
  // at four start alignments. ~35k probes -- fast even under ASan, and it is the
  // sweep that catches a wrong loop bound.
  for (const std::size_t align :
       {std::size_t{0}, std::size_t{1}, std::size_t{7}, std::size_t{31}}) {
    for (std::size_t len = 0; len <= 96; ++len) {
      for (std::size_t pos = 0; pos <= len; ++pos) {  // pos == len means "absent"
        auto buf = padded_buffer(96);
        if (pos < len) {
          buf[align + pos] = needle;
        }
        const std::string_view view{buf.data() + align, len};
        const std::size_t expected = (pos < len) ? pos : npos;

        INFO("align=" << align << " len=" << len << " pos=" << pos);
        REQUIRE(find_byte_naive(view, needle) == expected);
        REQUIRE(find_byte_libc(view, needle) == expected);
        REQUIRE(find_byte(view, needle) == expected);
#if SIMDFIX_HAS_AVX2
        if (simdfix::detail::has_avx2()) {
          REQUIRE(simdfix::kernels::find_byte_avx2(view, needle) == expected);
        }
#endif
      }
    }
  }
}

TEST_CASE("scans agree on large inputs at every alignment", "[scan]") {
  constexpr std::size_t size = 4096;

  for (std::size_t align = 0; align < 64; ++align) {
    for (const std::size_t pos : {std::size_t{0},
                                  std::size_t{1},
                                  std::size_t{31},
                                  std::size_t{32},
                                  std::size_t{33},
                                  std::size_t{2047},
                                  std::size_t{4094},
                                  std::size_t{4095}}) {
      auto buf = padded_buffer(size);
      buf[align + pos] = needle;
      const std::string_view view{buf.data() + align, size};

      INFO("align=" << align << " pos=" << pos);
      REQUIRE(find_byte_naive(view, needle) == pos);
      REQUIRE(find_byte_libc(view, needle) == pos);
      REQUIRE(find_byte(view, needle) == pos);
#if SIMDFIX_HAS_AVX2
      if (simdfix::detail::has_avx2()) {
        REQUIRE(simdfix::kernels::find_byte_avx2(view, needle) == pos);
      }
#endif
    }
  }
}

TEST_CASE("find_byte returns the first of many matches", "[scan]") {
  std::mt19937_64 rng{0xBEEFULL};
  for (int trial = 0; trial < 200; ++trial) {
    auto buf = padded_buffer(1024);
    const std::size_t align = rng() % 64;

    std::size_t first = npos;
    for (std::size_t i = 0; i < 1024; ++i) {
      if ((rng() % 32) == 0) {
        buf[align + i] = needle;
        if (first == npos) {
          first = i;
        }
      }
    }

    const std::string_view view{buf.data() + align, 1024};
    INFO("trial=" << trial << " align=" << align);
    REQUIRE(find_byte_naive(view, needle) == first);
    REQUIRE(find_byte_libc(view, needle) == first);
    REQUIRE(find_byte(view, needle) == first);
#if SIMDFIX_HAS_AVX2
    if (simdfix::detail::has_avx2()) {
      REQUIRE(simdfix::kernels::find_byte_avx2(view, needle) == first);
    }
#endif
  }
}

#if SIMDFIX_HAS_AVX2

TEST_CASE("block_mask_avx2 reports every delimiter in a 32-byte block", "[scan][avx2]") {
  if (!simdfix::detail::has_avx2()) {
    SUCCEED("AVX2 unavailable on this CPU; kernel not exercised");
    return;
  }

  std::mt19937_64 rng{0xF00DULL};
  for (int trial = 0; trial < 500; ++trial) {
    std::vector<char> block(32, filler);
    std::uint32_t expected = 0;
    for (std::uint32_t i = 0; i < 32; ++i) {
      if ((rng() % 4) == 0) {
        block[i] = needle;
        expected |= (std::uint32_t{1} << i);
      }
    }

    INFO("trial=" << trial);
    REQUIRE(simdfix::kernels::block_mask_avx2(block.data(), needle) == expected);
  }

  SECTION("bit 0 is the first byte, bit 31 the last") {
    std::vector<char> block(32, filler);
    block[0] = needle;
    CHECK(simdfix::kernels::block_mask_avx2(block.data(), needle) == 0x0000'0001U);

    block[0] = filler;
    block[31] = needle;
    CHECK(simdfix::kernels::block_mask_avx2(block.data(), needle) == 0x8000'0000U);

    const std::vector<char> all(32, needle);
    CHECK(simdfix::kernels::block_mask_avx2(all.data(), needle) == 0xFFFF'FFFFU);
  }
}

#endif  // SIMDFIX_HAS_AVX2

TEST_CASE("runtime dispatch reports which kernel it selected", "[scan][dispatch]") {
  const auto isa = simdfix::detail::active_isa();
  const std::string_view name = simdfix::detail::to_string(isa);
  INFO("active ISA: " << name);
  CHECK((name == "scalar" || name == "avx2"));
  CHECK((isa == simdfix::detail::isa::avx2) == simdfix::detail::has_avx2());
}
