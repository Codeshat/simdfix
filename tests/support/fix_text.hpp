// SPDX-License-Identifier: MIT
//
// Test-only helpers for writing FIX messages readably.
//
// Spelling SOH as `\x01` inside a test literal is both unreadable and a trap:
// `"\x0135="` is one hex escape, not SOH followed by `35=`. Writing `|` and
// translating is the only version that stays correct under editing.
#ifndef SIMDFIX_TESTS_FIX_TEXT_HPP
#define SIMDFIX_TESTS_FIX_TEXT_HPP

#include <array>
#include <cstdint>
#include <string>
#include <string_view>

#include "simdfix/checksum.hpp"
#include "simdfix/field.hpp"

namespace simdfix::test {

/// Replace every `|` with SOH.
[[nodiscard]] inline std::string fix(std::string text) {
  for (char& c : text) {
    if (c == '|') {
      c = soh;
    }
  }
  return text;
}

/// Wrap a pipe-delimited body in a correct BeginString, BodyLength and CheckSum.
/// The body must be field text only, e.g. `"35=D|55=AAPL|"`.
[[nodiscard]] inline std::string wrap(std::string_view body_text) {
  const std::string body = fix(std::string{body_text});

  std::string out = "8=FIX.4.2";
  out += soh;
  out += "9=" + std::to_string(body.size());
  out += soh;
  out += body;

  const std::uint8_t sum = kernels::checksum_scalar(out);
  std::array<char, 3> digits{};
  kernels::format_checksum(sum, digits.data());

  out += "10=";
  out.append(digits.data(), digits.size());
  out += soh;
  return out;
}

}  // namespace simdfix::test

#endif  // SIMDFIX_TESTS_FIX_TEXT_HPP
