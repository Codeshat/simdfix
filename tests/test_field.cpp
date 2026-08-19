// SPDX-License-Identifier: MIT
//
// Field value conversion. Mostly a test that the checked-accumulate integer
// parser rejects everything FIX says is invalid -- the interesting cases are
// all at the boundaries.

#include <cstdint>
#include <limits>
#include <string_view>
#include <type_traits>

#include <catch2/catch_test_macros.hpp>

#include "simdfix/field.hpp"

using simdfix::field;
using simdfix::parse_error;

TEST_CASE("parse_unsigned accepts plain decimal digits", "[field]") {
  using simdfix::detail::parse_unsigned;

  CHECK(parse_unsigned<std::uint32_t>("0") == 0U);
  CHECK(parse_unsigned<std::uint32_t>("7") == 7U);
  CHECK(parse_unsigned<std::uint32_t>("000042") == 42U);
  CHECK(parse_unsigned<std::uint32_t>("4294967295") == 4294967295U);
  CHECK(parse_unsigned<std::uint64_t>("18446744073709551615") ==
        std::numeric_limits<std::uint64_t>::max());
}

TEST_CASE("parse_unsigned rejects everything else", "[field]") {
  using simdfix::detail::parse_unsigned;

  const auto rejected = [](std::string_view s) {
    const auto r = parse_unsigned<std::uint32_t>(s);
    return !r.has_value() && r.error() == parse_error::bad_value;
  };

  CHECK(rejected(""));
  CHECK(rejected(" 1"));
  CHECK(rejected("1 "));
  CHECK(rejected("+1"));
  CHECK(rejected("-1"));
  CHECK(rejected("1.0"));
  CHECK(rejected("1e3"));
  CHECK(rejected("0x10"));
  CHECK(rejected("abc"));
  CHECK(rejected("12a"));

  SECTION("overflow is caught exactly at the boundary, not one past it") {
    CHECK(parse_unsigned<std::uint32_t>("4294967295").has_value());
    CHECK(rejected("4294967296"));
    CHECK(rejected("99999999999999999999"));
    // Leading zeros must not push a valid value over the cutoff.
    CHECK(parse_unsigned<std::uint32_t>("0000000004294967295") == 4294967295U);
  }
}

TEST_CASE("field::as_int handles both signs and the INT64_MIN asymmetry", "[field]") {
  constexpr auto int64_min = std::numeric_limits<std::int64_t>::min();
  constexpr auto int64_max = std::numeric_limits<std::int64_t>::max();

  CHECK(field{1, "0"}.as_int() == 0);
  CHECK(field{1, "-0"}.as_int() == 0);
  CHECK(field{1, "123"}.as_int() == 123);
  CHECK(field{1, "-123"}.as_int() == -123);
  CHECK(field{1, "9223372036854775807"}.as_int() == int64_max);

  // |INT64_MIN| is one larger than INT64_MAX; negating after parsing into a
  // signed type would be UB, which is why the magnitude is accumulated unsigned.
  CHECK(field{1, "-9223372036854775808"}.as_int() == int64_min);
  CHECK_FALSE(field{1, "9223372036854775808"}.as_int().has_value());
  CHECK_FALSE(field{1, "-9223372036854775809"}.as_int().has_value());
  CHECK_FALSE(field{1, "-"}.as_int().has_value());
  CHECK_FALSE(field{1, ""}.as_int().has_value());
}

TEST_CASE("field::as_char and as_bool are strict about width", "[field]") {
  CHECK(field{35, "D"}.as_char() == 'D');
  CHECK_FALSE(field{35, ""}.as_char().has_value());
  CHECK_FALSE(field{35, "AB"}.as_char().has_value());

  CHECK(field{43, "Y"}.as_bool() == true);
  CHECK(field{43, "N"}.as_bool() == false);
  CHECK_FALSE(field{43, "y"}.as_bool().has_value());
  CHECK_FALSE(field{43, "true"}.as_bool().has_value());
  CHECK_FALSE(field{43, ""}.as_bool().has_value());
}

TEST_CASE("field conversions are usable at compile time", "[field]") {
  // Not a formality: constexpr-ability is what proves the parse path has no
  // hidden allocation, no I/O and no UB the compiler can see.
  static_assert(field{38, "100"}.as_uint().value() == 100U);
  static_assert(field{44, "-5"}.as_int().value() == -5);
  static_assert(field{35, "D"}.as_char().value() == 'D');
  static_assert(!field{38, "x"}.as_uint().has_value());
  SUCCEED();
}

TEST_CASE("field is a two-view aggregate with value semantics", "[field]") {
  static_assert(std::is_trivially_copyable_v<field>);
  static_assert(std::is_aggregate_v<field>);
  CHECK(field{1, "a"} == field{1, "a"});
  CHECK_FALSE(field{1, "a"} == field{2, "a"});
  CHECK_FALSE(field{1, "a"} == field{1, "b"});
}
