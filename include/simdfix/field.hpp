// SPDX-License-Identifier: MIT
//
// A parsed FIX field: a numeric tag plus a view into the caller's buffer.
//
// `field` owns nothing. Every `field` handed out by simdfix points into the
// bytes the caller passed in, and is valid exactly as long as those bytes are.
// That is the whole zero-copy contract, and it is the first thing to say out
// loud when someone asks how the parser avoids allocation.
#ifndef SIMDFIX_FIELD_HPP
#define SIMDFIX_FIELD_HPP

#include <cstdint>
#include <limits>
#include <string_view>

#include "simdfix/error.hpp"

namespace simdfix {

/// FIX 4.2's field separator, SOH (ASCII 0x01).
inline constexpr char soh = '\x01';

/// FIX 4.2's tag/value separator.
inline constexpr char equals = '=';

namespace detail {

/// Parse an unsigned decimal integer with no sign, no whitespace, no leading-`+`.
/// Rejects the empty string and anything that overflows `T`.
///
/// Deliberately not `std::from_chars`: this runs per field, and the checked
/// accumulate below compiles to tighter code than the general-purpose routine
/// while rejecting exactly the inputs FIX says are invalid.
template<typename T>
[[nodiscard]] constexpr result<T> parse_unsigned(std::string_view s) noexcept {
  static_assert(std::numeric_limits<T>::is_integer && !std::numeric_limits<T>::is_signed);

  if (s.empty()) {
    return fail(parse_error::bad_value);
  }

  constexpr T limit = std::numeric_limits<T>::max();
  constexpr T cutoff = limit / 10;
  constexpr auto cutlim = static_cast<unsigned>(limit % 10);

  T acc = 0;
  for (const char c : s) {
    const auto digit = static_cast<unsigned>(static_cast<unsigned char>(c)) - unsigned{'0'};
    if (digit > 9U) {
      return fail(parse_error::bad_value);
    }
    if (acc > cutoff || (acc == cutoff && digit > cutlim)) {
      return fail(parse_error::bad_value);
    }
    acc = static_cast<T>(acc * 10) + static_cast<T>(digit);
  }
  return acc;
}

}  // namespace detail

/// A tag/value pair pointing into the caller's buffer.
struct field {
  std::uint32_t tag{};
  std::string_view value{};

  [[nodiscard]] constexpr bool operator==(const field&) const noexcept = default;

  /// The raw bytes between `=` and the next `<SOH>`. May legally be empty.
  [[nodiscard]] constexpr std::string_view as_string_view() const noexcept { return value; }

  /// Single-character values (MsgType, Side, OrdType, ...).
  [[nodiscard]] constexpr result<char> as_char() const noexcept {
    if (value.size() != 1) {
      return fail(parse_error::bad_value);
    }
    return value.front();
  }

  [[nodiscard]] constexpr result<std::uint64_t> as_uint() const noexcept {
    return detail::parse_unsigned<std::uint64_t>(value);
  }

  [[nodiscard]] constexpr result<std::int64_t> as_int() const noexcept {
    if (value.empty()) {
      return fail(parse_error::bad_value);
    }
    const bool negative = value.front() == '-';
    const std::string_view digits = negative ? value.substr(1) : value;

    const auto magnitude = detail::parse_unsigned<std::uint64_t>(digits);
    if (!magnitude) {
      return fail(magnitude.error());
    }

    constexpr auto max_positive =
        static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
    if (negative) {
      // |INT64_MIN| is one past INT64_MAX, so it needs its own arm.
      if (*magnitude > max_positive + 1) {
        return fail(parse_error::bad_value);
      }
      if (*magnitude == max_positive + 1) {
        return std::numeric_limits<std::int64_t>::min();
      }
      return -static_cast<std::int64_t>(*magnitude);
    }
    if (*magnitude > max_positive) {
      return fail(parse_error::bad_value);
    }
    return static_cast<std::int64_t>(*magnitude);
  }

  /// True when the value is `Y`, false when `N`; anything else is an error.
  [[nodiscard]] constexpr result<bool> as_bool() const noexcept {
    if (value.size() != 1) {
      return fail(parse_error::bad_value);
    }
    switch (value.front()) {
      case 'Y':
        return true;
      case 'N':
        return false;
      default:
        return fail(parse_error::bad_value);
    }
  }
};

/// The handful of tags simdfix itself has to reason about. This is not a
/// dictionary -- generating the full FIX 4.2 tag set is an explicit non-goal.
namespace tags {
inline constexpr std::uint32_t begin_string = 8;
inline constexpr std::uint32_t body_length = 9;
inline constexpr std::uint32_t check_sum = 10;
inline constexpr std::uint32_t msg_type = 35;
inline constexpr std::uint32_t msg_seq_num = 34;
inline constexpr std::uint32_t sender_comp_id = 49;
inline constexpr std::uint32_t target_comp_id = 56;
inline constexpr std::uint32_t sending_time = 52;
}  // namespace tags

}  // namespace simdfix

#endif  // SIMDFIX_FIELD_HPP
