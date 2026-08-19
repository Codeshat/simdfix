// SPDX-License-Identifier: MIT
//
// End-to-end parsing, decomposed so the cost is attributable.
//
//   frame        -- locate the message, validate structure. No second pass.
//   parse        -- frame plus a full checksum pass over the bytes.
//   iterate      -- parse, then walk every field.
//   find_one     -- parse, then look up a single tag (the common real workload:
//                   most consumers want Symbol and Side, not all 20 fields).
//
// Subtracting `frame` from `parse` gives the checksum's share; subtracting
// `parse` from `iterate` gives the delimiter scan's share. Reporting only the
// end-to-end number would make it impossible to tell which kernel earned what.

#include <cstddef>
#include <cstdint>
#include <string_view>

#include <benchmark/benchmark.h>

#include "bench_support.hpp"
#include "simdfix/detail/cpu.hpp"
#include "simdfix/parser.hpp"

namespace {

void set_rates(benchmark::State& state, std::size_t messages) {
  const auto bytes = static_cast<std::int64_t>(simdfix::bench::corpus_bytes().size());
  state.SetBytesProcessed(state.iterations() * bytes);
  state.SetItemsProcessed(state.iterations() * static_cast<std::int64_t>(messages));
  state.SetLabel(simdfix::detail::to_string(simdfix::detail::active_isa()));
}

void bm_frame(benchmark::State& state) {
  const auto& messages = simdfix::bench::corpus_messages();
  std::size_t ok = 0;

  for (auto _ : state) {
    for (const std::string_view msg : messages) {
      const auto framed = simdfix::frame(msg);
      simdfix::bench::sink(framed);
      ok += framed.has_value() ? 1U : 0U;
    }
  }

  set_rates(state, messages.size());
  if (ok != static_cast<std::size_t>(state.iterations()) * messages.size()) {
    state.SkipWithError("a message failed to frame; the benchmark is measuring the error path");
  }
}

void bm_parse(benchmark::State& state) {
  const auto& messages = simdfix::bench::corpus_messages();
  std::size_t ok = 0;

  for (auto _ : state) {
    for (const std::string_view msg : messages) {
      const auto parsed = simdfix::parse(msg);
      simdfix::bench::sink(parsed);
      ok += parsed.has_value() ? 1U : 0U;
    }
  }

  set_rates(state, messages.size());
  if (ok != static_cast<std::size_t>(state.iterations()) * messages.size()) {
    state.SkipWithError("a message failed to parse; the benchmark is measuring the error path");
  }
}

void bm_iterate(benchmark::State& state) {
  const auto& messages = simdfix::bench::corpus_messages();
  std::size_t fields = 0;

  for (auto _ : state) {
    for (const std::string_view msg : messages) {
      const auto parsed = simdfix::parse(msg);
      if (!parsed) {
        continue;
      }
      for (const simdfix::field& f : *parsed) {
        simdfix::bench::sink(f.tag);
        simdfix::bench::sink(f.value);
        ++fields;
      }
    }
  }

  set_rates(state, messages.size());
  state.counters["fields/s"] =
      benchmark::Counter(static_cast<double>(fields), benchmark::Counter::kIsRate);
}

void bm_find_one(benchmark::State& state) {
  const auto& messages = simdfix::bench::corpus_messages();

  for (auto _ : state) {
    for (const std::string_view msg : messages) {
      const auto parsed = simdfix::parse(msg);
      if (!parsed) {
        continue;
      }
      // Tag 55 (Symbol) sits well into the message, so this is not a
      // first-field lucky case.
      simdfix::bench::sink(parsed->find(55));
    }
  }

  set_rates(state, messages.size());
}

/// The realistic shape: bytes arrive as one blob and the reader splits them.
/// Includes the framing cursor work the per-message benchmarks skip.
void bm_reader_stream(benchmark::State& state) {
  const std::string_view buffer = simdfix::bench::corpus_bytes();
  std::size_t count = 0;

  for (auto _ : state) {
    simdfix::reader r{buffer};
    while (auto msg = r.next()) {
      simdfix::bench::sink(msg->raw());
      ++count;
    }
  }

  set_rates(state, simdfix::bench::corpus_messages().size());
  simdfix::bench::sink(count);
}

}  // namespace

BENCHMARK(bm_frame)->Name("parse/frame");
BENCHMARK(bm_parse)->Name("parse/parse");
BENCHMARK(bm_iterate)->Name("parse/iterate_all_fields");
BENCHMARK(bm_find_one)->Name("parse/find_symbol");
BENCHMARK(bm_reader_stream)->Name("parse/reader_stream");
