// SPDX-License-Identifier: MIT
//
// simdfix -- a zero-copy FIX 4.2 tagvalue parser.
//
// Umbrella header. Everything is header-only; there is nothing to link.
//
//   #include <simdfix/simdfix.hpp>
//
//   simdfix::reader r{bytes};
//   while (auto msg = r.next()) {
//     for (const simdfix::field& f : *msg) {
//       // f.value is a view into `bytes` -- no copy, no allocation.
//     }
//   }
//
// Scope, stated up front because unstated limitations read as ignorance:
//   * FIX 4.2 tagvalue encoding only. No FIXML, no SBE, no FAST.
//   * No repeating groups. Fields are yielded flat, in wire order.
//   * No session layer: no sequence-number tracking, no resend, no heartbeats.
//   * Parsing only. simdfix does not construct or serialise messages.
//   * No data dictionary. Values are returned as bytes; typing them is yours.
#ifndef SIMDFIX_SIMDFIX_HPP
#define SIMDFIX_SIMDFIX_HPP

#include "simdfix/checksum.hpp"
#include "simdfix/detail/cpu.hpp"
#include "simdfix/error.hpp"
#include "simdfix/field.hpp"
#include "simdfix/message.hpp"
#include "simdfix/parser.hpp"
#include "simdfix/scan.hpp"

#define SIMDFIX_VERSION_MAJOR 0
#define SIMDFIX_VERSION_MINOR 1
#define SIMDFIX_VERSION_PATCH 0
#define SIMDFIX_VERSION_STRING "0.1.0"

namespace simdfix {

[[nodiscard]] constexpr const char* version() noexcept {
  return SIMDFIX_VERSION_STRING;
}

/// Which kernel set this process will actually use: "scalar" or "avx2".
/// Print it next to any timing you report.
[[nodiscard]] inline const char* active_isa_name() noexcept {
  return detail::to_string(detail::active_isa());
}

}  // namespace simdfix

#endif  // SIMDFIX_SIMDFIX_HPP
