// SPDX-License-Identifier: MIT
//
// Writes a deterministic FIX 4.2 corpus to a file.
//
//   gen_corpus --count 100000 --seed 42 --out corpus/orders.fix
//
// The benchmarks generate their corpus in-process, so this tool exists for the
// cases that need bytes on disk: seeding the fuzzer, feeding `perf` a fixed
// workload, and letting anyone reproduce the exact input a README number came
// from.

#include <charconv>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

#include "simdfix/simdfix.hpp"
#include "simdfix_corpus/corpus.hpp"

namespace {

void usage(const char* argv0) {
  std::cerr << "usage: " << argv0 << " [options]\n"
            << "  --count N      messages to generate (default 10000)\n"
            << "  --seed N       PRNG seed (default 12648430)\n"
            << "  --exec-ratio F fraction of ExecutionReports, 0.0-1.0 (default 0.5)\n"
            << "  --out PATH     output file (default stdout)\n"
            << "  --split DIR    also write one file per message into DIR (fuzz seeds)\n";
}

template<typename T>
bool parse_number(std::string_view s, T& out) {
  const auto* first = s.data();
  const auto* last = s.data() + s.size();
  const auto res = std::from_chars(first, last, out);
  return res.ec == std::errc{} && res.ptr == last;
}

struct config {
  simdfix::corpus::options opts;
  std::string out_path;
  std::string split_dir;
};

/// Outcome of argument parsing: either run with `cfg`, or exit with `exit_code`.
struct parse_result {
  config cfg;
  bool should_run = true;
  int exit_code = 0;
};

parse_result parse_args(const char* argv0, const std::vector<std::string_view>& args) {
  parse_result result;
  const auto reject = [&result](std::string_view message) {
    std::cerr << message << "\n";
    result.should_run = false;
    result.exit_code = 2;
  };

  for (std::size_t i = 0; i < args.size(); ++i) {
    const std::string_view arg = args[i];
    const auto value = [&]() -> std::string_view {
      return (i + 1 < args.size()) ? args[++i] : std::string_view{};
    };

    if (arg == "--help" || arg == "-h") {
      usage(argv0);
      result.should_run = false;
      return result;
    }
    if (arg == "--count") {
      if (!parse_number(value(), result.cfg.opts.message_count)) {
        reject("bad --count");
        return result;
      }
    } else if (arg == "--seed") {
      if (!parse_number(value(), result.cfg.opts.seed)) {
        reject("bad --seed");
        return result;
      }
    } else if (arg == "--exec-ratio") {
      const std::string text{value()};
      char* end = nullptr;
      const double ratio = std::strtod(text.c_str(), &end);
      if (end == text.c_str() || ratio < 0.0 || ratio > 1.0) {
        reject("bad --exec-ratio");
        return result;
      }
      result.cfg.opts.exec_report_ratio = ratio;
    } else if (arg == "--out") {
      result.cfg.out_path = value();
    } else if (arg == "--split") {
      result.cfg.split_dir = value();
    } else {
      std::cerr << "unknown argument: " << arg << "\n";
      usage(argv0);
      result.should_run = false;
      result.exit_code = 2;
      return result;
    }
  }
  return result;
}

}  // namespace

int main(int argc, char** argv) {
  const std::vector<std::string_view> args(argv + 1, argv + argc);
  const parse_result parsed = parse_args(argv[0], args);
  if (!parsed.should_run) {
    return parsed.exit_code;
  }

  const simdfix::corpus::options& opts = parsed.cfg.opts;
  const std::string& out_path = parsed.cfg.out_path;
  const std::string& split_dir = parsed.cfg.split_dir;

  const std::string data = simdfix::corpus::generate(opts);

  // Never emit a corpus the library cannot read: a generator bug would
  // otherwise surface as a mysterious parser failure hours later.
  const auto messages = simdfix::corpus::split(data);
  if (messages.size() != opts.message_count) {
    std::cerr << "internal error: generated " << opts.message_count << " messages but only "
              << messages.size() << " parsed back\n";
    return 1;
  }

  if (out_path.empty()) {
    std::cout.write(data.data(), static_cast<std::streamsize>(data.size()));
  } else {
    std::ofstream file(out_path, std::ios::binary | std::ios::trunc);
    if (!file) {
      std::cerr << "cannot open " << out_path << " for writing\n";
      return 1;
    }
    file.write(data.data(), static_cast<std::streamsize>(data.size()));
    if (!file) {
      std::cerr << "write to " << out_path << " failed\n";
      return 1;
    }
  }

  if (!split_dir.empty()) {
    for (std::size_t i = 0; i < messages.size(); ++i) {
      const std::string path = split_dir + "/msg" + std::to_string(i) + ".fix";
      std::ofstream file(path, std::ios::binary | std::ios::trunc);
      if (!file) {
        std::cerr << "cannot open " << path << " for writing (does " << split_dir << " exist?)\n";
        return 1;
      }
      file.write(messages[i].data(), static_cast<std::streamsize>(messages[i].size()));
    }
  }

  std::cerr << "simdfix " << simdfix::version() << " [" << simdfix::active_isa_name() << "]: wrote "
            << messages.size() << " messages, " << data.size() << " bytes\n";
  return 0;
}
