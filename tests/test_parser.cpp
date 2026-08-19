// SPDX-License-Identifier: MIT
//
// Framing. This is the security-relevant part of the parser: BodyLength is
// attacker-controlled on any real feed, so every test here is really asking
// "does a lie about the length become a bounds violation?"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "simdfix/parser.hpp"
#include "support/fix_text.hpp"

using simdfix::frame;
using simdfix::parse;
using simdfix::parse_error;
using simdfix::reader;
using simdfix::test::fix;
using simdfix::test::wrap;

TEST_CASE("a valid message parses and exposes its fields", "[parser]") {
  const std::string raw = wrap("35=D|49=SENDER|56=TARGET|34=1|55=AAPL|54=1|38=100|");

  const auto msg = parse(raw);
  REQUIRE(msg.has_value());
  CHECK(msg->raw() == raw);
  CHECK(msg->size() == raw.size());
  CHECK(msg->msg_type() == "D");
  CHECK(msg->find(55)->value == "AAPL");
  CHECK(msg->validate_checksum().has_value());
  CHECK(msg->validate_fields().has_value());
}

TEST_CASE("every strict prefix of a valid message is incomplete, never a false parse",
          "[parser][framing]") {
  const std::string raw = wrap("35=D|49=SENDER|56=TARGET|34=1|55=AAPL|");

  // The property that matters for a stream reader: feeding bytes one at a time
  // must never produce a message early, and must never produce a hard error
  // that would trigger a spurious resynchronisation.
  for (std::size_t n = 0; n < raw.size(); ++n) {
    const std::string_view prefix{raw.data(), n};
    const auto msg = parse(prefix);
    INFO("prefix length " << n << " of " << raw.size());
    REQUIRE_FALSE(msg.has_value());
    REQUIRE(msg.error() == parse_error::incomplete);
  }

  REQUIRE(parse(raw).has_value());
}

TEST_CASE("BeginString is checked exactly", "[parser][framing]") {
  CHECK(parse("").error() == parse_error::incomplete);
  CHECK(parse("8").error() == parse_error::incomplete);
  CHECK(parse("8=FIX.4.").error() == parse_error::incomplete);

  CHECK(parse("9=FIX.4.2\x01").error() == parse_error::bad_begin_string);
  CHECK(parse("8=FIX.4.4\x01").error() == parse_error::bad_begin_string);
  CHECK(parse("8=FIX.4.2 ").error() == parse_error::bad_begin_string);
  CHECK(parse("garbage").error() == parse_error::bad_begin_string);
  // A prefix of the right length but wrong content is a hard error, not "wait".
  CHECK(parse("8=FIY").error() == parse_error::bad_begin_string);
}

TEST_CASE("BodyLength must be present and numeric", "[parser][framing]") {
  CHECK(frame(fix("8=FIX.4.2|35=D|")).error() == parse_error::bad_body_length);
  CHECK(frame(fix("8=FIX.4.2|9=abc|35=D|10=000|")).error() == parse_error::bad_body_length);
  CHECK(frame(fix("8=FIX.4.2|9=|35=D|10=000|")).error() == parse_error::bad_body_length);
  CHECK(frame(fix("8=FIX.4.2|9=-5|35=D|10=000|")).error() == parse_error::bad_body_length);
  CHECK(frame(fix("8=FIX.4.2|9= 5|35=D|10=000|")).error() == parse_error::bad_body_length);

  SECTION("an unbounded run of digits is malformed, not merely incomplete") {
    // Otherwise a hostile peer could pin a reader in "need more bytes" forever.
    const std::string flood = fix("8=FIX.4.2|9=") + std::string(64, '1');
    CHECK(frame(flood).error() == parse_error::bad_body_length);
  }

  SECTION("a plausible partial length is still incomplete") {
    CHECK(frame(fix("8=FIX.4.2|9=123")).error() == parse_error::incomplete);
  }
}

TEST_CASE("a lying BodyLength is caught, not trusted", "[parser][framing][security]") {
  const std::string good = wrap("35=D|55=AAPL|");

  SECTION("too short: the trailer is not where BodyLength says") {
    std::string bad = good;
    const std::size_t pos = bad.find("9=");
    REQUIRE(pos != std::string::npos);
    bad.replace(pos, 4, "9=4" + std::string(1, simdfix::soh));
    const auto msg = frame(bad);
    REQUIRE_FALSE(msg.has_value());
    CHECK(msg.error() == parse_error::body_length_mismatch);
  }

  SECTION("too long: reads past the buffer would be required, so it is incomplete") {
    std::string bad = good;
    const std::size_t pos = bad.find("9=");
    bad.replace(pos, 4, "9=9" + std::string(1, simdfix::soh));
    // BodyLength 9 lands short of the real trailer here; either way the parser
    // must not dereference past `bad.size()`. ASan in the debug-san preset is
    // what actually proves that.
    const auto msg = frame(bad);
    CHECK_FALSE(msg.has_value());
  }

  SECTION("an enormous BodyLength is incomplete, never an overflow") {
    const std::string bad = fix("8=FIX.4.2|9=4294967290|35=D|10=000|");
    CHECK(frame(bad).error() == parse_error::incomplete);
  }

  SECTION("a BodyLength above uint32 range is rejected outright") {
    const std::string bad = fix("8=FIX.4.2|9=99999999999|35=D|10=000|");
    CHECK(frame(bad).error() == parse_error::bad_body_length);
  }
}

TEST_CASE("the trailer must be a well-formed CheckSum field", "[parser][framing]") {
  // BodyLength 5 lands on `11=`, not on the CheckSum field. The buffer is long
  // enough that this is a genuine mismatch rather than a truncated read.
  CHECK(frame(fix("8=FIX.4.2|9=5|35=0|11=xxxx|")).error() == parse_error::body_length_mismatch);

  // Right shape, non-numeric digits.
  const std::string body = fix("35=0|");
  const std::string bad =
      fix("8=FIX.4.2|9=") + std::to_string(body.size()) + simdfix::soh + body + fix("10=abc|");
  CHECK(frame(bad).error() == parse_error::bad_checksum_field);

  // Right shape, missing the terminating SOH.
  const std::string unterminated =
      fix("8=FIX.4.2|9=") + std::to_string(body.size()) + simdfix::soh + body + "10=000";
  CHECK(frame(unterminated).error() == parse_error::incomplete);
}

TEST_CASE("frame accepts what parse rejects on checksum grounds", "[parser]") {
  std::string bad = wrap("35=D|55=AAPL|");
  // Corrupt the stated checksum without touching the framing.
  bad[bad.size() - 2] = (bad[bad.size() - 2] == '0') ? '1' : '0';

  CHECK(frame(bad).has_value());  // structure is still fine
  const auto msg = parse(bad);
  REQUIRE_FALSE(msg.has_value());
  CHECK(msg.error() == parse_error::checksum_mismatch);
}

TEST_CASE("a corrupted body is caught by the checksum", "[parser]") {
  std::string bad = wrap("35=D|55=AAPL|38=100|");
  const std::size_t pos = bad.find("AAPL");
  REQUIRE(pos != std::string::npos);
  bad[pos] = 'B';

  CHECK(parse(bad).error() == parse_error::checksum_mismatch);
}

TEST_CASE("reader drains a buffer of back-to-back messages", "[parser][reader]") {
  const std::string a = wrap("35=D|34=1|55=AAPL|");
  const std::string b = wrap("35=8|34=2|55=MSFT|");
  const std::string c = wrap("35=D|34=3|55=NVDA|");
  const std::string stream = a + b + c;

  reader r{stream};
  std::vector<std::string> types;
  std::size_t count = 0;
  while (auto msg = r.next()) {
    ++count;
    types.emplace_back(msg->msg_type().value());
  }

  CHECK(count == 3);
  CHECK(types == std::vector<std::string>{"D", "8", "D"});
  CHECK(r.done());
  CHECK(r.remaining().empty());
  CHECK(r.consumed() == stream.size());
}

TEST_CASE("reader stops without consuming on a partial tail", "[parser][reader]") {
  const std::string a = wrap("35=D|34=1|55=AAPL|");
  const std::string b = wrap("35=8|34=2|55=MSFT|");
  const std::string partial = b.substr(0, b.size() / 2);
  const std::string stream = a + partial;

  reader r{stream};
  REQUIRE(r.next().has_value());

  const auto next = r.next();
  REQUIRE_FALSE(next.has_value());
  CHECK(next.error() == parse_error::incomplete);

  // The leftover must be handed back intact so the caller can prepend it to the
  // next read. Getting this wrong is how stream parsers lose messages.
  CHECK(r.remaining() == partial);
  CHECK(r.consumed() == a.size());
}

TEST_CASE("reader does not advance past a hard error", "[parser][reader]") {
  const std::string good = wrap("35=D|34=1|");
  const std::string stream = good + "garbage-not-fix";

  reader r{stream};
  REQUIRE(r.next().has_value());

  const auto bad = r.next();
  REQUIRE_FALSE(bad.has_value());
  CHECK(bad.error() == parse_error::bad_begin_string);
  CHECK(r.remaining() == "garbage-not-fix");

  // Resynchronisation is the caller's policy decision, so it is an explicit call.
  r.skip(r.remaining().size());
  CHECK(r.done());
  CHECK(r.next().error() == parse_error::incomplete);
}

TEST_CASE("reader::skip is clamped to the remaining bytes", "[parser][reader]") {
  const std::string stream = wrap("35=D|");
  reader r{stream};
  r.skip(1'000'000);
  CHECK(r.done());
  CHECK(r.remaining().empty());
}

TEST_CASE("parsing never depends on NUL termination", "[parser]") {
  // A message sitting in the middle of a larger receive buffer must parse
  // identically to one that ends the buffer.
  const std::string msg = wrap("35=D|55=AAPL|");
  const std::string padded = msg + std::string(64, 'Z');

  const auto a = parse(msg);
  const auto b = parse(padded);
  REQUIRE(a.has_value());
  REQUIRE(b.has_value());
  CHECK(a->size() == b->size());
  CHECK(a->raw() == b->raw());
}
