# simdfix

A zero-copy FIX 4.2 tagvalue parser in C++23, with runtime-dispatched AVX2 kernels
and a benchmark suite measured against [hffix](https://github.com/jamesdbrock/hffix).

Header-only. No allocation on the parse path. No exceptions on the parse path.
Every field is a `std::string_view` into the caller's buffer.

```cpp
#include <simdfix/simdfix.hpp>

simdfix::reader r{bytes};              // bytes is a std::string_view over your socket buffer
while (auto msg = r.next()) {          // frames one message, verifies its checksum
  for (const simdfix::field& f : *msg) {
    // f.tag is a std::uint32_t, f.value is a view into `bytes`. Nothing was copied.
  }
}
// r.remaining() is the partial tail; prepend it to your next read.
```

---

## Status

Both AVX2 kernels are written, equivalence-tested at every length and alignment,
differentially fuzzed against their scalar references, and benchmarked.

They did not both pay off, and the README says so:

- **Kernel 1 (checksum) is a large win.** 9.25 → 102 GiB/s on a 4 KiB buffer, and
  5.98 → 9.35 GiB/s on real ~250-byte corpus messages. Full `parse()` throughput
  went from 2.35 to 4.26 GiB/s.
- **Kernel 2 (delimiter scan) wins on long buffers and *loses* on the workload
  that matters.** 2.4 → 67 GiB/s versus the naive loop on a 4 KiB scan, but on the
  field walk it runs at 0.79 GiB/s against 1.42 GiB/s for the inlined scalar loop.
  A `target("avx2")` function cannot be inlined into a baseline caller, so every
  call is a real call — and the parser makes two of them per ~12-byte field. The
  fix is amortisation, not a faster scan; see [Roadmap](#roadmap).

The numbers in this README are reproducible from this repo with the commands given.
None of them are estimates.

---

## Non-goals

Stated explicitly, because these are decisions rather than omissions:

- **FIX 4.2 tagvalue only.** No FIXML, no SBE, no FAST. No other FIX versions —
  the framing is identical for 4.4/5.0, but claiming support without a conformance
  suite would be a claim this repo cannot back.
- **No repeating groups.** Fields are yielded flat, in wire order. Correct group
  handling needs a data dictionary, which is a different project.
- **No session layer.** No sequence-number tracking, no resend requests, no
  heartbeats, no logon state machine.
- **Parsing only.** simdfix does not construct or serialise messages. The corpus
  generator in `tools/` writes FIX, and it deliberately lives outside the library.
- **No data dictionary.** Values come back as bytes with helpers to convert them
  (`as_int`, `as_uint`, `as_char`, `as_bool`); knowing that tag 44 is a price is
  the caller's job.

---

## Requirements

| | |
|---|---|
| Compiler | GCC ≥ 12, or **Clang ≥ 19** |
| Standard | C++23 (`std::expected` is the error-handling story) |
| Build | CMake ≥ 3.25, Ninja |
| CPU | Any x86-64. AVX2 is selected at run time, never required |

> **Clang 18 and earlier will not work with libstdc++.** libstdc++ gates
> `<expected>` behind `__cpp_concepts >= 202002L`, and Clang only began defining
> that in 19 — so Clang 18 has no `std::expected` at any `-std=` level, however new
> the libstdc++ is. This bites `clangd` too: **clangd-18 flags every use of
> `std::expected` as an error** even though GCC compiles the file cleanly. Install
> `clangd-19` or newer. `CMakeLists.txt` fails at configure time with this
> explanation rather than letting you discover it through template spew.

```bash
sudo apt install -y ninja-build g++-13 clang-19 clangd-19 clang-format-19 clang-tidy-19
```

---

## Build and test

```bash
cmake --preset debug-san      # -O1 -g, ASan + UBSan  (the development default)
cmake --build --preset debug-san
ctest --preset debug-san
```

| Preset | What it is for |
|---|---|
| `debug-san` | ASan + UBSan, warnings as errors. Use this while developing. |
| `debug` | `-O0 -g`, no sanitizers, for stepping in a debugger. |
| `release` | `-O3 -DNDEBUG`, portable baseline x86-64. Builds benchmarks. |
| `release-native` | Adds `-march=native`. **Benchmarks only** — see below. |
| `relwithdebinfo` | `-O2 -g` with frame pointers, for `perf`. |
| `fuzz` | libFuzzer + sanitizers. Needs Clang: `CC=clang-19 CXX=clang++-19 cmake --preset fuzz` |

`compile_commands.json` is symlinked into the repo root at configure time, so
clangd works with no editor configuration once any preset has been configured.

---

## Why runtime dispatch, and why `release-native` is for benchmarks only

Building the whole project with `-march=native` produces a binary that dies with
SIGILL on any CPU older than the one that compiled it. That is why the default
`release` preset targets baseline x86-64 and each vector kernel instead carries
`__attribute__((target("avx2")))`, which lets one function be compiled for AVX2
inside an otherwise-baseline translation unit. `simdfix::detail::has_avx2()` picks
the path once, via `__builtin_cpu_supports`, cached in a function-local static.

`release-native` exists to answer "what would this cost if the compiler could
assume my exact CPU?" — a useful upper bound, and not something to distribute.

Two consequences worth knowing:

- A `target("avx2")` function **cannot be inlined into a baseline caller**. Every
  dispatched kernel call is a real call. For a 4 KiB checksum that is free; for a
  12-byte delimiter scan it is most of the cost. This is the central tension in
  kernel 2 and the reason the field-walk benchmark exists.
- `SIMDFIX_DISABLE_AVX2=1` forces the scalar path in an already-built binary, so
  scalar and vector can be compared without a rebuild. CI runs the whole suite
  both ways, because otherwise a scalar-path regression would ship green from
  every AVX2-capable runner.

---

## Benchmarks

```bash
cmake --preset release && cmake --build --preset release
./build/release/bin/simdfix_bench --benchmark_repetitions=5 \
                                  --benchmark_report_aggregates_only=true
```

**Measured on:** Intel i5-10210U (Comet Lake, AVX2, no AVX-512), Ubuntu 24.04,
GCC 13.3, `-O3` baseline x86-64, corpus of 20,000 generated messages (mean 196.7
bytes: NewOrderSingle and ExecutionReport, 50/50). Median of 5 repetitions.

> **Caveat, stated because it changes how much these numbers are worth:** this is a
> laptop with CPU frequency scaling and ASLR enabled, and Google Benchmark warns
> about both. Run-to-run variation of 20–30% is normal here. Treat these as ratios
> on one machine, not as absolute throughput.

### Kernel 1 — checksum

`vpsadbw` against zero reduces 32 bytes to four 64-bit partial sums in one
instruction. Four accumulators over a 128-byte main loop, scalar tail.

| Bytes | scalar | AVX2 | |
|---|---|---|---|
| 16 | **4.27 GiB/s** | 2.88 GiB/s | scalar wins — the dispatched call is not free |
| 32 | 6.46 GiB/s | **9.26 GiB/s** | crossover |
| 128 | 8.75 GiB/s | **29.75 GiB/s** | 3.4× |
| 1024 | 9.22 GiB/s | **84.02 GiB/s** | 9.1× |
| 4096 | 9.25 GiB/s | **102.44 GiB/s** | 11.1× |
| corpus messages (~250 B) | 5.98 GiB/s | **9.35 GiB/s** | 1.56× |

The 16-byte row is the interesting one: below the crossover the vector kernel is
*slower*, because a `target("avx2")` function cannot be inlined into a baseline
caller, so the dispatched call is a real call. Real messages are far above the
crossover, which is why the parser dispatches unconditionally rather than
branching on length.

### Kernel 2 — delimiter scan

One `vpcmpeqb` + `vpmovmskb` per 32 bytes; 128-byte unrolled loop that ORs the
comparison vectors so the no-hit iteration costs one `vpmovmskb` and one branch.

| Bytes | naive | `memchr` | AVX2 |
|---|---|---|---|
| 64 | 2.55 | 11.18 | **17.64** GiB/s |
| 512 | 2.36 | 53.49 | **62.66** GiB/s |
| 4096 | 2.46 | **129.35** | 67.42 GiB/s |
| 65536 | 2.42 | **89.55** | 77.63 GiB/s |

**And on the workload that actually matters, it loses:**

| `scan/field_walk` (one scan per delimiter, ~12 bytes each) | Throughput |
|---|---|
| naive byte loop | **1.42 GiB/s** |
| `memchr` | 0.79 GiB/s |
| AVX2 | 0.79 GiB/s |

Same cause as the 16-byte checksum row, but here it is not an edge case — it is
the whole use case. The parser makes two non-inlinable calls per ~12-byte field,
and the call overhead swamps twelve byte-comparisons that the scalar loop inlines
into the caller. `memchr` loses for the same reason, at 0.55× the naive loop.

So the win for kernel 2 is not a faster scan. It is doing more work per call:
extracting *every* delimiter position from one 32-byte block and consuming them
with `tzcnt`, amortising one call over ~8 fields instead of paying two per field.
`block_mask_avx2` is that primitive, and it is written and fuzzed; wiring
`field_iterator` to consume it is the next commit. See [Roadmap](#roadmap).

### Where the time goes

| Benchmark | Before kernels | Now | Reading |
|---|---|---|---|
| `parse/frame` | 4.76 GiB/s | **6.52 GiB/s** | Framing: header, BodyLength, trailer check |
| `parse/parse` | 2.35 GiB/s | **4.26 GiB/s** | Framing + checksum |
| `parse/reader_stream` | 2.66 GiB/s | **4.45 GiB/s** | Full stream split, the realistic shape |
| `parse/find_symbol` | 577 MiB/s | 0.85 GiB/s | Parse + one linear tag lookup |
| `parse/iterate_all_fields` | 340 MiB/s | 0.51 GiB/s | Parse + walk every field |

Kernel 1 closed most of the `parse` vs `frame` gap: the checksum used to cost
half of all throughput and now costs about a third. Field iteration remains ~13×
more expensive than framing, so **field iteration is still where the parser
spends its time** — and it is the part kernel 2 has not yet helped.

### Against hffix (v1.4.1)

Like-for-like: hffix's `message_reader` validates framing but does not verify the
checksum, so it is compared against `simdfix::frame`, which does the same work.

| Workload | simdfix | hffix | |
|---|---|---|---|
| Frame only | 6.76 GiB/s | **9.54 GiB/s** | hffix 1.41× faster |
| Frame + read every field | 0.63 GiB/s | **1.44 GiB/s** | hffix 2.29× faster |

**simdfix still loses to hffix on both**, and the gap on this machine widened
rather than closed — both hffix numbers also moved up between measurement rounds,
which is exactly the run-to-run variation the caveat above warns about. What has
not changed is the shape of the deficit: it is field iteration, and it is the
per-call cost of dispatch, not the cost of scanning bytes.

---

## Correctness

| | |
|---|---|
| Unit tests | 45 cases, ~103,000 assertions (Catch2 v3.15.3) |
| Sanitizers | ASan + UBSan on every test in CI, `-fno-sanitize-recover=undefined` |
| Kernel equivalence | Every length 0–4 KiB × all 64 start alignments, vector vs scalar |
| Differential fuzzing | `fuzz_kernels` — any divergence from the scalar reference is a bug |
| Structural fuzzing | `fuzz_parse` under ASan; asserts every view stays inside the input |
| Dispatch coverage | Whole suite re-run with `SIMDFIX_DISABLE_AVX2=1` |
| Warnings | `-Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Werror` |

```bash
CC=clang-19 CXX=clang++-19 cmake --preset fuzz && cmake --build --preset fuzz
./build/fuzz/bin/fuzz_parse   -max_total_time=60
./build/fuzz/bin/fuzz_kernels -max_total_time=60
```

**Framing is the security-relevant part.** BodyLength is attacker-controlled on any
real feed, so simdfix trusts it only far enough to *locate* the trailer, then
verifies the trailer is actually there. A stream that lies about BodyLength gets
`body_length_mismatch`, not a buffer overrun. `fuzz_parse` asserts on every
iteration that returned views lie inside the input buffer.

### Error handling

`simdfix::result<T>` is `std::expected<T, parse_error>`; `parse_error` is a
byte-sized enum. A failed parse costs a branch, not an unwind, and never allocates.

`parse_error::incomplete` is the only non-fatal error — it means "supply more bytes
and retry". Everything else means the stream is corrupt at this offset. The reader
does not advance past a hard error, so resynchronisation stays the caller's policy
decision (`reader::skip`).

### Lifetimes

Every entry point takes a `std::string_view` and returns views into those same
bytes, which makes this a dangling read:

```cpp
auto msg = simdfix::parse(build_message());   // temporary dies at the semicolon
```

The parameters are annotated `[[clang::lifetimebound]]`, so **Clang rejects that at
compile time** while still allowing `parse(build_message()).has_value()`, where the
temporary outlives its use. GCC has no equivalent yet; ASan catches the same bug at
run time in `debug-san`. Both mechanisms found real instances of this in the test
suite while it was being written.

---

## Layout

```
include/simdfix/     header-only library
  detail/cpu.hpp     CPU detection, target attributes, runtime dispatch
  error.hpp          parse_error, result<T> = std::expected<T, parse_error>
  field.hpp          field view + checked numeric conversion
  checksum.hpp       kernel 1: scalar + AVX2 + dispatch
  scan.hpp           kernel 2: naive + memchr + AVX2 + dispatch
  message.hpp        message_view, field_iterator
  parser.hpp         framing: frame(), parse(), reader
tests/               Catch2, one file per header
bench/               Google Benchmark, incl. the hffix baseline
tools/               deterministic corpus generator
fuzz/                libFuzzer targets
```

`field_iterator` is declared an **input** iterator, not forward, because it stashes
the current field inside itself — two equal iterators would not return references
to the same object, which is exactly the multipass guarantee `forward_iterator`
makes. Claiming forward would be a lie the standard library is entitled to rely on.

---

## Roadmap

1. ~~**Kernel 1** — AVX2 checksum via `_mm256_sad_epu8`.~~ Done: 11.1× at 4 KiB,
   1.56× on real messages.
2. ~~**Kernel 2** — AVX2 delimiter scan producing a 32-bit mask per block.~~
   Written and fuzzed. Wins on long buffers, loses on the field walk.
3. **Amortise the field walk over `block_mask_avx2`.** The measured problem is
   per-call dispatch overhead, not scan speed, so the fix is to batch: pull a
   window of delimiter offsets out of `field_iterator`'s remaining bytes with one
   AVX2 call and pop them as fields are consumed, instead of two calls per field.
   Constraint worth stating up front: `field_iterator` is copied on
   post-increment, so the cached window has to stay small (~8 offsets), which is
   why this is a design change rather than a one-line swap.
4. Close the field-iteration gap against hffix. Step 3 is the mechanism.
5. AVX-512 kernels behind the same dispatch (untestable on this CPU — so it stays
   on the roadmap rather than in the code).
6. Tag-indexed lookup for callers that read many fields from one message.

---

## Licence

MIT. See [LICENSE](LICENSE).

`hffix` (BSD-2-Clause) is fetched only for benchmark comparison and is not a
dependency of the library.
