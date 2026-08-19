// SPDX-License-Identifier: MIT
//
// Field iteration and lookup over an already-framed message.

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "simdfix/field.hpp"
#include "simdfix/message.hpp"
#include "support/fix_text.hpp"

using simdfix::field;
using simdfix::message_view;
using simdfix::parse_error;
using simdfix::test::fix;

TEST_CASE("iteration yields every field in wire order", "[message]") {
  const std::string raw = fix("8=FIX.4.2|9=12|35=D|55=AAPL|10=123|");
  const auto msg = message_view::unchecked(raw);

  std::vector<field> fields;
  for (const field& f : msg) {
    fields.push_back(f);
  }

  REQUIRE(fields.size() == 5);
  CHECK(fields[0] == field{8, "FIX.4.2"});
  CHECK(fields[1] == field{9, "12"});
  CHECK(fields[2] == field{35, "D"});
  CHECK(fields[3] == field{55, "AAPL"});
  CHECK(fields[4] == field{10, "123"});

  // The whole point: values borrow the caller's bytes, they are not copies.
  CHECK(fields[3].value.data() >= raw.data());
  CHECK(fields[3].value.data() < raw.data() + raw.size());
}

TEST_CASE("values may be empty and may contain unusual bytes", "[message]") {
  const std::string raw = fix("8=FIX.4.2|9=9|58=|112=a=b|10=000|");
  const auto msg = message_view::unchecked(raw);

  const auto text = msg.find(58);
  REQUIRE(text.has_value());
  CHECK(text->value.empty());

  // Only the *first* `=` separates tag from value; later ones are data.
  const auto test_req = msg.find(112);
  REQUIRE(test_req.has_value());
  CHECK(test_req->value == "a=b");
}

TEST_CASE("find returns the first match and reports absence", "[message]") {
  const std::string raw = fix("8=FIX.4.2|9=1|35=D|55=AAPL|55=MSFT|10=000|");
  const auto msg = message_view::unchecked(raw);

  const auto sym = msg.find(55);
  REQUIRE(sym.has_value());
  CHECK(sym->value == "AAPL");

  const auto missing = msg.find(9999);
  REQUIRE_FALSE(missing.has_value());
  CHECK(missing.error() == parse_error::field_not_found);
}

TEST_CASE("msg_type and msg_seq_num read the well-known tags", "[message]") {
  const std::string raw = fix("8=FIX.4.2|9=1|35=8|34=42|10=000|");
  const auto msg = message_view::unchecked(raw);

  CHECK(msg.msg_type() == "8");
  CHECK(msg.msg_seq_num() == 42U);
}

TEST_CASE("iteration stops at a malformed field and says why", "[message]") {
  SECTION("no equals sign") {
    const std::string raw = fix("8=FIX.4.2|9=1|nonsense|10=000|");
    const auto msg = message_view::unchecked(raw);

    auto it = msg.begin();
    std::size_t seen = 0;
    while (it != msg.end()) {
      ++seen;
      ++it;
    }
    CHECK(seen == 2);  // tags 8 and 9 parsed before the bad field
    REQUIRE(it.error().has_value());
    CHECK(*it.error() == parse_error::malformed_field);
    CHECK(msg.validate_fields().error() == parse_error::malformed_field);
  }

  // NOTE: each of these binds the bytes to a named string first. Passing
  // `fix(...)` straight into unchecked() compiles, but the temporary dies at
  // the end of the statement and leaves the view dangling -- ASan reports it as
  // stack-use-after-scope, and Clang rejects it outright thanks to the
  // lifetimebound annotation on the parameter.
  SECTION("empty tag") {
    const std::string raw = fix("=value|");
    CHECK(message_view::unchecked(raw).validate_fields().error() == parse_error::malformed_field);
  }

  SECTION("non-numeric tag") {
    const std::string raw = fix("8=FIX.4.2|abc=1|");
    CHECK(message_view::unchecked(raw).validate_fields().error() == parse_error::bad_tag);
  }

  SECTION("tag wider than uint32") {
    const std::string raw = fix("99999999999=1|");
    CHECK(message_view::unchecked(raw).validate_fields().error() == parse_error::bad_tag);
  }

  SECTION("unterminated final field") {
    const auto msg = message_view::unchecked("8=FIX.4.2");
    CHECK(msg.validate_fields().error() == parse_error::malformed_field);
  }

  SECTION("a well-formed message validates clean") {
    const std::string raw = fix("8=FIX.4.2|9=5|35=0|10=000|");
    CHECK(message_view::unchecked(raw).validate_fields().has_value());
  }
}

TEST_CASE("an empty message_view is an empty range", "[message]") {
  const message_view msg;
  CHECK(msg.empty());
  CHECK(msg.begin() == msg.end());
  CHECK(msg.validate_fields().has_value());
  CHECK(msg.find(35).error() == parse_error::field_not_found);
}

TEST_CASE("checksum_region excludes the trailer", "[message]") {
  const std::string raw = fix("8=FIX.4.2|9=5|35=0|10=123|");
  const auto msg = message_view::unchecked(raw);

  CHECK(msg.checksum_region() == fix("8=FIX.4.2|9=5|35=0|"));
  CHECK(msg.checksum_region().size() == raw.size() - simdfix::trailer_size);
  CHECK(msg.stated_checksum() == 123U);
}
