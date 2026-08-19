// SPDX-License-Identifier: MIT
//
// Structural fuzzing of the framing path.
//
// The threat model is a peer that controls every byte, including BodyLength.
// Run under ASan/UBSan (the `fuzz` preset does), so the pass condition is not
// "does not crash" but "performs no out-of-bounds access, no UB, and no
// allocation-driven blowup, on any input".
//
//   cmake --preset fuzz && cmake --build --preset fuzz
//   ./build/fuzz/bin/fuzz_parse -max_total_time=60 corpus/

#include <cstddef>
#include <cstdint>
#include <string_view>

#include "simdfix/simdfix.hpp"

namespace {

void abort_if(bool condition, const char* /*why*/) {
  if (condition) {
    __builtin_trap();
  }
}

/// Consume a `[[nodiscard]]` result without inspecting it.
///
/// The value is uninteresting -- the point is that the accessor *ran*, on bytes
/// the fuzzer chose, under ASan and UBSan. Casting to void would say the same
/// thing to a reader but lets the optimiser delete the call and makes
/// clang-tidy's unused-return-value check (correctly) complain, so the result
/// is folded into a volatile counter instead.
template<typename T>
void observe(const T& value) noexcept {
  static volatile std::uint64_t sink = 0;
  sink += value.has_value() ? 1U : 0U;
}

}  // namespace

// NOLINTNEXTLINE(readability-identifier-naming): the name is libFuzzer's ABI.
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  const std::string_view buffer{reinterpret_cast<const char*>(data), size};

  simdfix::reader r{buffer};
  std::size_t guard = 0;

  while (true) {
    const auto msg = r.next();
    if (!msg) {
      break;
    }

    // Every view handed back must lie strictly inside the input buffer. This is
    // the invariant that a lying BodyLength would violate.
    abort_if(msg->raw().data() < buffer.data(), "message starts before the buffer");
    abort_if(msg->raw().data() + msg->size() > buffer.data() + buffer.size(),
             "message ends past the buffer");
    abort_if(msg->empty(), "a parsed message must be non-empty, or the reader cannot advance");

    // Touch every accessor, so a fault in any of them is reachable.
    for (const simdfix::field& f : *msg) {
      abort_if(f.value.data() < buffer.data(), "field value starts before the buffer");
      abort_if(f.value.data() + f.value.size() > buffer.data() + buffer.size(),
               "field value ends past the buffer");
      observe(f.as_int());
      observe(f.as_uint());
      observe(f.as_char());
      observe(f.as_bool());
    }

    observe(msg->validate_fields());
    observe(msg->msg_type());
    observe(msg->msg_seq_num());
    observe(msg->find(55));
    observe(msg->find(0));

    // parse() already verified the checksum, so this must agree.
    abort_if(!msg->validate_checksum().has_value(),
             "parse() returned a message that fails its own"
             " checksum");

    // The reader must make progress on every successful parse; otherwise a
    // crafted input could spin forever.
    abort_if(++guard > size + 1, "reader failed to advance");
  }

  return 0;
}
