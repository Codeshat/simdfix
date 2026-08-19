// SPDX-License-Identifier: MIT
//
// External baseline: hffix (https://github.com/jamesdbrock/hffix), pinned at
// v1.4.1.
//
// Why an external baseline at all: "simdfix's AVX2 kernel beats simdfix's own
// scalar kernel" is a self-referential claim. hffix is a respected, genuinely
// zero-allocation, in-place tagvalue FIX parser with the same design goals, so
// it is the honest question -- is this faster than what a careful engineer
// would already reach for?
//
// Fairness notes, because a benchmark against another library is only worth
// anything if the comparison is like-for-like:
//   * hffix's message_reader validates framing but does NOT verify the
//     checksum, so `hffix/frame_only` is compared against `simdfix::frame`.
//     `simdfix::parse` does strictly more work; both are reported.
//   * Both parsers see the same bytes from the same corpus.
//   * Both are driven to the same observable result -- reading a message, or
//     reading every field's tag and value -- so neither gets to skip work the
//     other performs.

#include <cstddef>
#include <cstdint>
#include <string_view>

#include <benchmark/benchmark.h>
#include <hffix.hpp>

#include "bench_support.hpp"
#include "simdfix/detail/cpu.hpp"
#include "simdfix/parser.hpp"

namespace {

void set_rates(benchmark::State& state, std::size_t messages) {
  const auto bytes = static_cast<std::int64_t>(simdfix::bench::corpus_bytes().size());
  state.SetBytesProcessed(state.iterations() * bytes);
  state.SetItemsProcessed(state.iterations() * static_cast<std::int64_t>(messages));
}

/// hffix: frame every message in the buffer, touching nothing inside them.
void hffix_frame_only(benchmark::State& state) {
  const std::string_view buffer = simdfix::bench::corpus_bytes();
  std::size_t count = 0;

  for (auto _ : state) {
    hffix::message_reader reader(buffer.data(), buffer.data() + buffer.size());
    for (; reader.is_complete(); reader = reader.next_message_reader()) {
      if (!reader.is_valid()) {
        break;
      }
      simdfix::bench::sink(reader.message_size());
      ++count;
    }
  }

  set_rates(state, simdfix::bench::corpus_messages().size());
  simdfix::bench::sink(count);
}

/// hffix: frame every message and read every field's tag and value.
void hffix_iterate(benchmark::State& state) {
  const std::string_view buffer = simdfix::bench::corpus_bytes();
  std::size_t fields = 0;

  for (auto _ : state) {
    hffix::message_reader reader(buffer.data(), buffer.data() + buffer.size());
    for (; reader.is_complete(); reader = reader.next_message_reader()) {
      if (!reader.is_valid()) {
        break;
      }
      for (auto it = reader.begin(); it != reader.end(); ++it) {
        simdfix::bench::sink(it->tag());
        simdfix::bench::sink(it->value().size());
        ++fields;
      }
    }
  }

  set_rates(state, simdfix::bench::corpus_messages().size());
  state.counters["fields/s"] =
      benchmark::Counter(static_cast<double>(fields), benchmark::Counter::kIsRate);
}

/// simdfix doing exactly what `hffix_frame_only` does, for a like-for-like row.
void simdfix_frame_only(benchmark::State& state) {
  const std::string_view buffer = simdfix::bench::corpus_bytes();
  std::size_t count = 0;

  for (auto _ : state) {
    simdfix::reader r{buffer};
    while (auto msg = r.next_unverified()) {
      simdfix::bench::sink(msg->size());
      ++count;
    }
  }

  set_rates(state, simdfix::bench::corpus_messages().size());
  state.SetLabel(simdfix::detail::to_string(simdfix::detail::active_isa()));
  simdfix::bench::sink(count);
}

/// simdfix doing exactly what `hffix_iterate` does.
void simdfix_iterate(benchmark::State& state) {
  const std::string_view buffer = simdfix::bench::corpus_bytes();
  std::size_t fields = 0;

  for (auto _ : state) {
    simdfix::reader r{buffer};
    while (auto msg = r.next_unverified()) {
      for (const simdfix::field& f : *msg) {
        simdfix::bench::sink(f.tag);
        simdfix::bench::sink(f.value.size());
        ++fields;
      }
    }
  }

  set_rates(state, simdfix::bench::corpus_messages().size());
  state.SetLabel(simdfix::detail::to_string(simdfix::detail::active_isa()));
  state.counters["fields/s"] =
      benchmark::Counter(static_cast<double>(fields), benchmark::Counter::kIsRate);
}

}  // namespace

BENCHMARK(hffix_frame_only)->Name("baseline/hffix/frame_only");
BENCHMARK(simdfix_frame_only)->Name("baseline/simdfix/frame_only");
BENCHMARK(hffix_iterate)->Name("baseline/hffix/iterate_all_fields");
BENCHMARK(simdfix_iterate)->Name("baseline/simdfix/iterate_all_fields");
