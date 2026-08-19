// SPDX-License-Identifier: MIT
//
// Deterministic FIX 4.2 corpus generation for tests and benchmarks.
//
// This is test scaffolding, not part of the library: it lives outside
// include/simdfix/, it allocates freely, and it is the one place in the repo
// that *builds* FIX rather than reading it. Keeping it separate is what lets
// the library keep "zero allocation on the parse path" as an unqualified claim.
//
// Determinism matters more than realism here. A fixed seed means a benchmark
// number from your laptop and one from CI describe the same work, and a fuzz
// crash reproduces.
#ifndef SIMDFIX_CORPUS_HPP
#define SIMDFIX_CORPUS_HPP

#include <array>
#include <cstddef>
#include <cstdint>
#include <random>
#include <string>
#include <string_view>
#include <vector>

#include "simdfix/checksum.hpp"
#include "simdfix/field.hpp"
#include "simdfix/parser.hpp"

namespace simdfix::corpus {

/// Wrap an already-assembled body (every field terminated by SOH, no
/// BeginString/BodyLength/CheckSum) into a complete, valid FIX 4.2 message.
[[nodiscard]] inline std::string finalize(std::string_view body) {
  std::string out;
  out.reserve(body.size() + 32);
  out += "8=FIX.4.2\x01";
  out += "9=";
  out += std::to_string(body.size());
  out += soh;
  out += body;

  // Deliberately the scalar kernel: the corpus must stay correct even while the
  // AVX2 checksum is being rewritten, or a kernel bug would silently produce a
  // matching corpus and the tests would agree with themselves.
  const std::uint8_t sum = kernels::checksum_scalar(out);
  std::array<char, 3> digits{};
  kernels::format_checksum(sum, digits.data());

  out += "10=";
  out.append(digits.data(), digits.size());
  out += soh;
  return out;
}

namespace detail {

inline void append_field(std::string& body, std::uint32_t tag, std::string_view value) {
  body += std::to_string(tag);
  body += equals;
  body += value;
  body += soh;
}

inline void append_field(std::string& body, std::uint32_t tag, std::uint64_t value) {
  append_field(body, tag, std::to_string(value));
}

inline std::string timestamp(std::uint32_t n) {
  // FIX UTCTimestamp, YYYYMMDD-HH:MM:SS.sss. Varying it keeps the field from
  // being a constant the branch predictor and the allocator both learn.
  std::string s = "20260817-";
  const std::uint32_t hh = (n / 3600) % 24;
  const std::uint32_t mm = (n / 60) % 60;
  const std::uint32_t ss = n % 60;
  const std::uint32_t ms = (n * 7) % 1000;
  const auto pad = [&s](std::uint32_t v, int width) {
    std::string d = std::to_string(v);
    while (static_cast<int>(d.size()) < width) {
      d.insert(d.begin(), '0');
    }
    s += d;
  };
  pad(hh, 2);
  s += ':';
  pad(mm, 2);
  s += ':';
  pad(ss, 2);
  s += '.';
  pad(ms, 3);
  return s;
}

inline constexpr std::array<std::string_view, 8> symbols{
    "AAPL", "MSFT", "GOOG", "AMZN", "NVDA", "TSLA", "BRK.B", "SPY"};

}  // namespace detail

/// NewOrderSingle (35=D). ~150-200 bytes, the shape that dominates order flow.
[[nodiscard]] inline std::string new_order_single(std::uint32_t seq, std::mt19937_64& rng) {
  std::string body;
  body.reserve(192);
  detail::append_field(body, 35, "D");
  detail::append_field(body, 49, "SIMDFIX");
  detail::append_field(body, 56, "EXCHANGE");
  detail::append_field(body, 34, seq);
  detail::append_field(body, 52, detail::timestamp(seq));
  detail::append_field(body, 11, "ORD" + std::to_string(1'000'000U + seq));
  detail::append_field(body, 21, "1");
  detail::append_field(body, 55, detail::symbols[rng() % detail::symbols.size()]);
  detail::append_field(body, 54, (rng() % 2) == 0 ? "1" : "2");
  detail::append_field(body, 60, detail::timestamp(seq));
  detail::append_field(body, 38, 100U + (rng() % 9900U));
  detail::append_field(body, 40, "2");
  detail::append_field(
      body, 44, std::to_string(10U + (rng() % 990U)) + "." + std::to_string(10U + (rng() % 89U)));
  detail::append_field(body, 59, "0");
  return finalize(body);
}

/// ExecutionReport (35=8). Longer and field-denser than a NewOrderSingle, which
/// makes it the more demanding case for the delimiter scan.
[[nodiscard]] inline std::string execution_report(std::uint32_t seq, std::mt19937_64& rng) {
  const auto qty = 100U + (rng() % 9900U);
  const auto filled = rng() % (qty + 1);

  std::string body;
  body.reserve(288);
  detail::append_field(body, 35, "8");
  detail::append_field(body, 49, "EXCHANGE");
  detail::append_field(body, 56, "SIMDFIX");
  detail::append_field(body, 34, seq);
  detail::append_field(body, 52, detail::timestamp(seq));
  detail::append_field(body, 37, "EXEC" + std::to_string(500'000U + seq));
  detail::append_field(body, 11, "ORD" + std::to_string(1'000'000U + seq));
  detail::append_field(body, 17, "FILL" + std::to_string(seq));
  // Tags 150 (ExecType) and 39 (OrdStatus) share an encoding here: 0 = New,
  // 1 = PartiallyFilled, 2 = Filled.
  std::string_view ord_status = "1";
  if (filled == 0) {
    ord_status = "0";
  } else if (filled == qty) {
    ord_status = "2";
  }
  detail::append_field(body, 150, ord_status);
  detail::append_field(body, 39, ord_status);
  detail::append_field(body, 55, detail::symbols[rng() % detail::symbols.size()]);
  detail::append_field(body, 54, (rng() % 2) == 0 ? "1" : "2");
  detail::append_field(body, 38, qty);
  detail::append_field(body, 14, filled);
  detail::append_field(body, 151, qty - filled);
  detail::append_field(body, 6, std::to_string(10U + (rng() % 990U)) + ".00");
  detail::append_field(body, 31, std::to_string(10U + (rng() % 990U)) + ".00");
  detail::append_field(body, 32, filled);
  detail::append_field(body, 60, detail::timestamp(seq));
  return finalize(body);
}

struct options {
  std::size_t message_count = 10'000;
  std::uint64_t seed = 0xC0FFEEULL;
  /// Fraction of messages that are ExecutionReports; the rest NewOrderSingles.
  double exec_report_ratio = 0.5;
};

/// One flat buffer of back-to-back messages, exactly as it would arrive on a
/// socket. Callers hand this straight to `simdfix::reader`.
[[nodiscard]] inline std::string generate(const options& opts = {}) {
  std::mt19937_64 rng{opts.seed};
  const auto threshold = static_cast<std::uint64_t>(opts.exec_report_ratio *
                                                    static_cast<double>(std::mt19937_64::max()));

  std::string out;
  out.reserve(opts.message_count * 256);
  for (std::size_t i = 0; i < opts.message_count; ++i) {
    const auto seq = static_cast<std::uint32_t>(i + 1);
    out += (rng() < threshold) ? execution_report(seq, rng) : new_order_single(seq, rng);
  }
  return out;
}

/// The same corpus, split into one view per message. Useful for benchmarks that
/// want to time single-message parsing without the reader's cursor logic.
[[nodiscard]] inline std::vector<std::string_view> split(std::string_view buffer) {
  std::vector<std::string_view> out;
  simdfix::reader r{buffer};
  while (auto msg = r.next()) {
    out.push_back(msg->raw());
  }
  return out;
}

}  // namespace simdfix::corpus

#endif  // SIMDFIX_CORPUS_HPP
