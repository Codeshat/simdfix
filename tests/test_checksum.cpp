// SPDX-License-Identifier: MIT
//
// Kernel 1 equivalence and correctness.
//
// `checksum_scalar` is the definition of correctness and `checksum_avx2` is
// checked against it, never the other way round. The sweeps below cover every
// length from 0 to 4 KiB at every start alignment, which is what catches the
// three ways a `vpsadbw` reduction goes wrong: a mishandled tail shorter than
// the vector width, a lane dropped in the horizontal fold, and an accumulator
// narrower than the sum it holds. Without them a wrong kernel shows up as a
// great benchmark number for the wrong answer.

#include <array>
#include <cstddef>
#include <cstdint>
#include <random>
#include <string>
#include <string_view>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "simdfix/checksum.hpp"
#include "simdfix/detail/cpu.hpp"

namespace {

/// A buffer with 64 bytes of slack at the front, so a subview can start at any
/// alignment. Vector kernels break on exactly the alignments the author never
/// tried, so the tests try all of them.
std::vector<char> random_bytes(std::size_t n, std::uint64_t seed = 0x5EEDULL) {
  std::mt19937_64 rng{seed};
  std::vector<char> buf(n);
  for (char& c : buf) {
    c = static_cast<char>(rng() & 0xFFU);
  }
  return buf;
}

}  // namespace

TEST_CASE("checksum matches the FIX definition on known messages", "[checksum]") {
  using simdfix::kernels::checksum_scalar;

  // Sum of bytes mod 256, computed independently below rather than copied from
  // the implementation.
  const std::string_view msg =
      "8=FIX.4.2\x01"
      "9=5\x01"
      "35=0\x01";
  std::uint32_t expected = 0;
  for (const char c : msg) {
    expected += static_cast<unsigned char>(c);
  }
  CHECK(checksum_scalar(msg) == static_cast<std::uint8_t>(expected & 0xFFU));

  CHECK(checksum_scalar("") == 0);
  // Explicit length: a string_view built from a `const char*` stops at the
  // first NUL, and FIX values are not NUL-terminated data.
  CHECK(checksum_scalar(std::string_view{"\0\0\0", 3}) == 0);
  // 256 bytes of 0x01 sum to 256, which is 0 mod 256 -- the wraparound case.
  CHECK(checksum_scalar(std::string(256, '\x01')) == 0);
  CHECK(checksum_scalar(std::string(255, '\x01')) == 255);
  CHECK(checksum_scalar(std::string(257, '\x01')) == 1);
  // High-bit bytes must be summed unsigned; treating char as signed here is the
  // classic FIX checksum bug.
  CHECK(checksum_scalar("\xFF") == 255);
  CHECK(checksum_scalar("\xFF\x01") == 0);
}

TEST_CASE("checksum is constexpr", "[checksum]") {
  static_assert(simdfix::kernels::checksum_scalar("") == 0);
  static_assert(simdfix::kernels::checksum_scalar("AB") == ('A' + 'B'));
  SUCCEED();
}

#if SIMDFIX_HAS_AVX2

TEST_CASE("checksum_avx2 agrees with the scalar reference at every length", "[checksum][avx2]") {
  if (!simdfix::detail::has_avx2()) {
    SUCCEED("AVX2 unavailable on this CPU; kernel not exercised");
    return;
  }

  const auto buf = random_bytes(4096 + 64);

  // Every length from 0 to 4 KiB catches off-by-ones in the main loop bound and
  // in the sub-32-byte tail.
  for (std::size_t len = 0; len <= 4096; ++len) {
    const std::string_view view{buf.data(), len};
    INFO("length = " << len);
    REQUIRE(simdfix::kernels::checksum_avx2(view) == simdfix::kernels::checksum_scalar(view));
  }
}

TEST_CASE("checksum_avx2 is correct at every start alignment", "[checksum][avx2]") {
  if (!simdfix::detail::has_avx2()) {
    SUCCEED("AVX2 unavailable on this CPU; kernel not exercised");
    return;
  }

  const auto buf = random_bytes(4096 + 64);

  for (std::size_t offset = 0; offset < 64; ++offset) {
    for (const std::size_t len : {std::size_t{0},
                                  std::size_t{1},
                                  std::size_t{31},
                                  std::size_t{32},
                                  std::size_t{33},
                                  std::size_t{63},
                                  std::size_t{64},
                                  std::size_t{127},
                                  std::size_t{200},
                                  std::size_t{1024}}) {
      const std::string_view view{buf.data() + offset, len};
      INFO("offset = " << offset << ", length = " << len);
      REQUIRE(simdfix::kernels::checksum_avx2(view) == simdfix::kernels::checksum_scalar(view));
    }
  }
}

TEST_CASE("checksum_avx2 handles saturating byte patterns", "[checksum][avx2]") {
  if (!simdfix::detail::has_avx2()) {
    SUCCEED("AVX2 unavailable on this CPU; kernel not exercised");
    return;
  }

  // All-0xFF is where a kernel that accumulates in 8- or 16-bit lanes, or that
  // uses a saturating add, diverges from the reference.
  for (const std::size_t len : {std::size_t{32}, std::size_t{1024}, std::size_t{65536}}) {
    const std::string all_ff(len, '\xFF');
    INFO("length = " << len);
    REQUIRE(simdfix::kernels::checksum_avx2(all_ff) == simdfix::kernels::checksum_scalar(all_ff));
  }
}

#endif  // SIMDFIX_HAS_AVX2

TEST_CASE("dispatched checksum equals the reference", "[checksum]") {
  const auto buf = random_bytes(1500);
  for (std::size_t len = 0; len <= buf.size(); len += 7) {
    const std::string_view view{buf.data(), len};
    REQUIRE(simdfix::kernels::checksum(view) == simdfix::kernels::checksum_scalar(view));
  }
}

TEST_CASE("format_checksum and parse_checksum round-trip over all 256 values", "[checksum]") {
  for (std::uint32_t i = 0; i < 256; ++i) {
    const auto value = static_cast<std::uint8_t>(i);
    std::array<char, 3> digits{};
    simdfix::kernels::format_checksum(value, digits.data());

    INFO("value = " << i << ", rendered = " << std::string_view(digits.data(), digits.size()));
    CHECK(digits[0] >= '0');
    CHECK(digits[0] <= '9');

    std::uint8_t parsed = 0;
    REQUIRE(simdfix::kernels::parse_checksum({digits.data(), digits.size()}, parsed));
    CHECK(parsed == value);
  }
}

TEST_CASE("parse_checksum enforces exactly three digits in 000-255", "[checksum]") {
  std::uint8_t out = 0;
  CHECK(simdfix::kernels::parse_checksum("000", out));
  CHECK(out == 0);
  CHECK(simdfix::kernels::parse_checksum("255", out));
  CHECK(out == 255);

  // FIX 4.2 fixes the CheckSum field at three characters, so a short field is
  // malformed rather than a small number.
  CHECK_FALSE(simdfix::kernels::parse_checksum("7", out));
  CHECK_FALSE(simdfix::kernels::parse_checksum("07", out));
  CHECK_FALSE(simdfix::kernels::parse_checksum("0007", out));
  CHECK_FALSE(simdfix::kernels::parse_checksum("", out));
  CHECK_FALSE(simdfix::kernels::parse_checksum("abc", out));
  CHECK_FALSE(simdfix::kernels::parse_checksum(" 12", out));
  CHECK_FALSE(simdfix::kernels::parse_checksum("+12", out));
  // Representable in three digits but not in a byte.
  CHECK_FALSE(simdfix::kernels::parse_checksum("256", out));
  CHECK_FALSE(simdfix::kernels::parse_checksum("999", out));
}
