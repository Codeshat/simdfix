// SPDX-License-Identifier: MIT
//
// Error type and result aliases.
//
// simdfix never throws on the parse path and never allocates to report an
// error. `parse_error` is a byte-sized enum and `simdfix::result<T>` is
// `std::expected<T, parse_error>`, so a failed parse costs a branch, not an
// unwind. Exceptions are reserved for genuine programmer error (out-of-range
// access on a validated message), which is why they do not appear here at all.
#ifndef SIMDFIX_ERROR_HPP
#define SIMDFIX_ERROR_HPP

#include <cstdint>
#include <expected>
#include <string_view>

namespace simdfix {

enum class parse_error : std::uint8_t {
  /// The buffer holds a prefix of a valid message; supply more bytes and retry.
  /// This is the only error a stream reader should treat as non-fatal.
  incomplete,

  /// Message does not begin with `8=FIX.4.2<SOH>`.
  bad_begin_string,

  /// Tag 9 (BodyLength) missing, non-numeric, or absent where required.
  bad_body_length,

  /// BodyLength does not land on the `10=` trailer -- the message is framed
  /// wrong, which on a stream means resynchronisation is required.
  body_length_mismatch,

  /// Tag 10 (CheckSum) missing or not exactly three digits.
  bad_checksum_field,

  /// Tag 10 is well-formed but disagrees with the computed checksum.
  checksum_mismatch,

  /// A field had no `=` before its `<SOH>`, or an empty tag.
  malformed_field,

  /// A tag was non-numeric or wider than a 32-bit unsigned.
  bad_tag,

  /// A value expected to be numeric was not, or overflowed its target type.
  bad_value,

  /// A lookup found no field with the requested tag.
  field_not_found,
};

[[nodiscard]] constexpr std::string_view to_string(parse_error e) noexcept {
  switch (e) {
    case parse_error::incomplete:
      return "incomplete";
    case parse_error::bad_begin_string:
      return "bad_begin_string";
    case parse_error::bad_body_length:
      return "bad_body_length";
    case parse_error::body_length_mismatch:
      return "body_length_mismatch";
    case parse_error::bad_checksum_field:
      return "bad_checksum_field";
    case parse_error::checksum_mismatch:
      return "checksum_mismatch";
    case parse_error::malformed_field:
      return "malformed_field";
    case parse_error::bad_tag:
      return "bad_tag";
    case parse_error::bad_value:
      return "bad_value";
    case parse_error::field_not_found:
      return "field_not_found";
  }
  return "unknown";
}

/// A value, or the reason it could not be produced.
template<typename T>
using result = std::expected<T, parse_error>;

/// Success, or the reason an operation failed.
using status = std::expected<void, parse_error>;

[[nodiscard]] constexpr std::unexpected<parse_error> fail(parse_error e) noexcept {
  return std::unexpected{e};
}

}  // namespace simdfix

#endif  // SIMDFIX_ERROR_HPP
