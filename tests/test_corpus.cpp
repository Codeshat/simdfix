// SPDX-License-Identifier: MIT
//
// End-to-end: the generated corpus must survive a full round trip.
//
// This is the test that makes the benchmark numbers trustworthy. If the corpus
// generator and the parser disagree, every throughput figure in the README is
// measuring the wrong work.

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "simdfix/parser.hpp"
#include "simdfix/simdfix.hpp"
#include "simdfix_corpus/corpus.hpp"

using simdfix::parse_error;

TEST_CASE("every generated message parses, validates and round-trips", "[corpus][e2e]") {
  simdfix::corpus::options opts;
  opts.message_count = 2'000;
  const std::string data = simdfix::corpus::generate(opts);

  simdfix::reader r{data};
  std::size_t count = 0;
  std::size_t fields_seen = 0;

  while (auto msg = r.next()) {
    ++count;

    // parse() already verified the checksum; assert it again against the raw
    // definition so a bug in the dispatched kernel cannot hide behind itself.
    REQUIRE(msg->validate_fields().has_value());
    REQUIRE(msg->computed_checksum() == simdfix::kernels::checksum_scalar(msg->checksum_region()));

    // Sequence numbers are generated 1..N, so any reordering or dropped message
    // shows up here rather than as a mysteriously fast benchmark.
    REQUIRE(msg->msg_seq_num() == count);

    const auto type = msg->msg_type();
    REQUIRE(type.has_value());
    REQUIRE((*type == "D" || *type == "8"));

    // BodyLength must describe the bytes between the header and the trailer.
    const auto stated_len = msg->find(simdfix::tags::body_length);
    REQUIRE(stated_len.has_value());
    const auto len = stated_len->as_uint();
    REQUIRE(len.has_value());
    const std::size_t header = simdfix::begin_string.size() + 2 + stated_len->value.size() + 1;
    REQUIRE(msg->size() == header + *len + simdfix::trailer_size);

    for ([[maybe_unused]] const simdfix::field& f : *msg) {
      ++fields_seen;
    }
  }

  CHECK(count == opts.message_count);
  CHECK(r.done());
  CHECK(r.consumed() == data.size());
  CHECK(fields_seen > count * 10);
}

TEST_CASE("generation is deterministic for a given seed", "[corpus]") {
  simdfix::corpus::options opts;
  opts.message_count = 100;
  opts.seed = 1234;

  const std::string a = simdfix::corpus::generate(opts);
  const std::string b = simdfix::corpus::generate(opts);
  CHECK(a == b);

  opts.seed = 5678;
  const std::string c = simdfix::corpus::generate(opts);
  CHECK(a != c);
}

TEST_CASE("corpus::split agrees with the reader", "[corpus]") {
  simdfix::corpus::options opts;
  opts.message_count = 500;
  const std::string data = simdfix::corpus::generate(opts);

  const auto messages = simdfix::corpus::split(data);
  REQUIRE(messages.size() == opts.message_count);

  std::size_t total = 0;
  for (const std::string_view m : messages) {
    total += m.size();
    REQUIRE(simdfix::parse(m).has_value());
    // Each view must point into the original buffer -- no copies anywhere.
    REQUIRE(m.data() >= data.data());
    REQUIRE(m.data() + m.size() <= data.data() + data.size());
  }
  CHECK(total == data.size());
}

TEST_CASE("a corpus with a single flipped bit fails the checksum", "[corpus][e2e]") {
  simdfix::corpus::options opts;
  opts.message_count = 1;
  std::string data = simdfix::corpus::generate(opts);
  REQUIRE(simdfix::parse(data).has_value());

  // Flip one bit in the middle of the body. Byte-sum checksums are weak, but
  // they do catch a single-bit change, and this pins the failure mode.
  const std::size_t mid = data.size() / 2;
  data[mid] = static_cast<char>(data[mid] ^ 0x01);

  const auto msg = simdfix::parse(data);
  if (msg.has_value()) {
    // The flip may have landed inside BodyLength's digits, which is a framing
    // error rather than a checksum error; either way it must not parse clean.
    FAIL("corrupted message parsed as valid");
  } else {
    CHECK((msg.error() == parse_error::checksum_mismatch ||
           msg.error() == parse_error::body_length_mismatch ||
           msg.error() == parse_error::bad_body_length || msg.error() == parse_error::incomplete));
  }
}
