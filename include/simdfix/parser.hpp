// SPDX-License-Identifier: MIT
//
// Framing: turning a byte buffer into message_views.
//
// FIX 4.2 is self-delimiting through BodyLength (tag 9): the message is
// `8=FIX.4.2<SOH>9=N<SOH>` followed by exactly N bytes followed by
// `10=NNN<SOH>`. simdfix trusts that length only far enough to *locate* the
// trailer, then verifies the trailer is really there. A stream that lies about
// BodyLength gets `body_length_mismatch`, not a buffer overrun -- which is the
// distinction between a parser and a vulnerability.
#ifndef SIMDFIX_PARSER_HPP
#define SIMDFIX_PARSER_HPP

#include <cstddef>
#include <cstdint>
#include <string_view>

#include "simdfix/checksum.hpp"
#include "simdfix/detail/cpu.hpp"
#include "simdfix/error.hpp"
#include "simdfix/field.hpp"
#include "simdfix/message.hpp"
#include "simdfix/scan.hpp"

namespace simdfix {

/// The only BeginString simdfix accepts. Supporting 4.4/5.0 is a non-goal; the
/// tagvalue framing is identical, but claiming support without a conformance
/// suite would be a claim this repo cannot back up.
inline constexpr std::string_view begin_string = "8=FIX.4.2\x01";

/// `9=` -- BodyLength always immediately follows BeginString in a valid message.
inline constexpr std::string_view body_length_prefix = "9=";

/// Guards against a garbage stream that presents an absurd number of digits
/// where BodyLength should be. Anything longer is malformed, not "incomplete",
/// so a stream reader can resynchronise instead of buffering forever.
inline constexpr std::size_t max_body_length_digits = 10;

/// Locate and structurally validate the first message in `buf`, *without*
/// verifying the checksum.
///
/// Split out from `parse()` so the benchmark suite can separate framing cost
/// from checksum cost, and so callers whose transport already guarantees
/// integrity (a replayed capture, a trusted shared-memory ring) can skip a full
/// second pass over the bytes.
[[nodiscard]] inline result<message_view> frame(
    std::string_view buf SIMDFIX_LIFETIMEBOUND) noexcept {
  // --- BeginString ------------------------------------------------------
  if (buf.size() < begin_string.size()) {
    // A prefix of a valid header is "not yet"; anything else is "never".
    return fail(begin_string.substr(0, buf.size()) == buf ? parse_error::incomplete
                                                          : parse_error::bad_begin_string);
  }
  if (buf.substr(0, begin_string.size()) != begin_string) {
    return fail(parse_error::bad_begin_string);
  }

  // --- BodyLength -------------------------------------------------------
  std::size_t pos = begin_string.size();
  if (buf.size() < pos + body_length_prefix.size()) {
    return fail(parse_error::incomplete);
  }
  if (buf.substr(pos, body_length_prefix.size()) != body_length_prefix) {
    return fail(parse_error::bad_body_length);
  }
  pos += body_length_prefix.size();

  const std::string_view digits_region = buf.substr(pos);
  const std::size_t digits_end = kernels::find_byte(digits_region, soh);
  if (digits_end == kernels::npos) {
    return fail(digits_region.size() > max_body_length_digits ? parse_error::bad_body_length
                                                              : parse_error::incomplete);
  }
  if (digits_end > max_body_length_digits) {
    return fail(parse_error::bad_body_length);
  }

  // std::uint32_t caps BodyLength at ~4 GiB, so the additions below cannot
  // overflow a 64-bit std::size_t. That is the point of the narrow type.
  const auto body_length =
      detail::parse_unsigned<std::uint32_t>(digits_region.substr(0, digits_end));
  if (!body_length) {
    return fail(parse_error::bad_body_length);
  }

  // --- Trailer ----------------------------------------------------------
  const std::size_t body_begin = pos + digits_end + 1;
  const std::size_t trailer_begin = body_begin + *body_length;
  const std::size_t total = trailer_begin + trailer_size;
  if (buf.size() < total) {
    return fail(parse_error::incomplete);
  }

  const std::string_view trailer = buf.substr(trailer_begin, trailer_size);
  if (trailer.substr(0, 3) != "10=") {
    // BodyLength pointed somewhere other than the checksum field.
    return fail(parse_error::body_length_mismatch);
  }
  if (trailer.back() != soh) {
    return fail(parse_error::bad_checksum_field);
  }
  std::uint8_t stated = 0;
  if (!kernels::parse_checksum(trailer.substr(3, 3), stated)) {
    return fail(parse_error::bad_checksum_field);
  }

  return message_view::unchecked(buf.substr(0, total));
}

/// Frame the first message in `buf` and verify its checksum.
///
/// This is the entry point. On `parse_error::incomplete` the caller should read
/// more bytes and retry with a longer buffer; every other error means the
/// stream is corrupt at this offset.
[[nodiscard]] inline result<message_view> parse(
    std::string_view buf SIMDFIX_LIFETIMEBOUND) noexcept {
  const auto framed = frame(buf);
  if (!framed) {
    return framed;
  }
  if (const auto ok = framed->validate_checksum(); !ok) {
    return fail(ok.error());
  }
  return framed;
}

/// Splits a buffer holding zero or more back-to-back messages.
///
/// Holds no buffer of its own: feed it a view of your receive ring, drain it,
/// and hand the leftover `remaining()` bytes to the next read. Errors do not
/// advance the cursor, so the caller decides whether to resynchronise or drop
/// the connection.
class reader {
 public:
  reader() = default;

  explicit reader(std::string_view buf SIMDFIX_LIFETIMEBOUND) noexcept
      : rest_(buf), begin_(buf.data()) {}

  /// Next complete message, or the reason there isn't one.
  /// `parse_error::incomplete` on an empty or partial tail is the normal
  /// termination condition, not a failure.
  [[nodiscard]] result<message_view> next() noexcept {
    if (rest_.empty()) {
      return fail(parse_error::incomplete);
    }
    const auto msg = parse(rest_);
    if (!msg) {
      return msg;
    }
    rest_.remove_prefix(msg->size());
    return msg;
  }

  /// Same as `next()` but skips checksum verification. See `frame()`.
  [[nodiscard]] result<message_view> next_unverified() noexcept {
    if (rest_.empty()) {
      return fail(parse_error::incomplete);
    }
    const auto msg = frame(rest_);
    if (!msg) {
      return msg;
    }
    rest_.remove_prefix(msg->size());
    return msg;
  }

  /// Bytes not yet consumed. Carry these into the next read.
  [[nodiscard]] std::string_view remaining() const noexcept { return rest_; }

  [[nodiscard]] std::size_t consumed() const noexcept {
    return begin_ == nullptr ? 0 : static_cast<std::size_t>(rest_.data() - begin_);
  }

  [[nodiscard]] bool done() const noexcept { return rest_.empty(); }

  /// Drop `n` bytes -- the resynchronisation primitive after a hard error.
  void skip(std::size_t n) noexcept { rest_.remove_prefix(n < rest_.size() ? n : rest_.size()); }

 private:
  std::string_view rest_{};
  const char* begin_ = nullptr;
};

}  // namespace simdfix

#endif  // SIMDFIX_PARSER_HPP
