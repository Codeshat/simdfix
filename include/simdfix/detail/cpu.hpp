// SPDX-License-Identifier: MIT
//
// CPU feature detection and target-attribute plumbing for runtime dispatch.
//
// simdfix is compiled for baseline x86-64 and selects the AVX2 kernels at run
// time. That is a deliberate choice: building the whole translation unit with
// `-mavx2` produces a binary that SIGILLs on any pre-Haswell CPU, and letting
// the compiler emit AVX2 into a function the dispatcher might not call is
// exactly the bug that class of build produces. Instead each vector kernel
// carries `__attribute__((target("avx2")))`, which lets one function be
// compiled for AVX2 inside an otherwise-baseline TU.
#ifndef SIMDFIX_DETAIL_CPU_HPP
#define SIMDFIX_DETAIL_CPU_HPP

#include <cstdint>
#include <cstdlib>
#include <cstring>

#if defined(__x86_64__) || defined(_M_X64)
#define SIMDFIX_X86 1
#else
#define SIMDFIX_X86 0
#endif

// Whether an AVX2 kernel can be *compiled* into this build. Whether it is
// *executed* is a separate, runtime question -- see simdfix::detail::has_avx2().
#if SIMDFIX_X86 && (defined(__GNUC__) || defined(__clang__))
#define SIMDFIX_HAS_AVX2 1
#define SIMDFIX_TARGET_AVX2 __attribute__((target("avx2,bmi,bmi2,lzcnt")))
#else
#define SIMDFIX_HAS_AVX2 0
#define SIMDFIX_TARGET_AVX2
#endif

// Every simdfix entry point takes a std::string_view and hands back views into
// those same bytes, which makes `auto m = simdfix::parse(make_string());` a
// dangling read: the temporary dies at the end of the full expression while `m`
// still points into it. Marking the parameter lifetimebound lets Clang's
// -Wdangling diagnose exactly that at compile time, while still permitting the
// legitimate `parse(make_string()).has_value()` where the temporary outlives
// its use. GCC has no equivalent yet, so it is a no-op there; ASan catches the
// same bug at run time in the debug-san preset.
#if defined(__clang__) && __has_cpp_attribute(clang::lifetimebound)
#define SIMDFIX_LIFETIMEBOUND [[clang::lifetimebound]]
#else
#define SIMDFIX_LIFETIMEBOUND
#endif

#if defined(__GNUC__) || defined(__clang__)
#define SIMDFIX_ALWAYS_INLINE inline __attribute__((always_inline))
#define SIMDFIX_LIKELY(x) __builtin_expect(!!(x), 1)
#define SIMDFIX_UNLIKELY(x) __builtin_expect(!!(x), 0)
#else
#define SIMDFIX_ALWAYS_INLINE inline
#define SIMDFIX_LIKELY(x) (x)
#define SIMDFIX_UNLIKELY(x) (x)
#endif

namespace simdfix::detail {

/// Which instruction set the dispatched kernels will use on this machine.
enum class isa : std::uint8_t {
  scalar,
  avx2,
};

namespace impl {

inline bool detect_avx2() noexcept {
  // Escape hatch: `SIMDFIX_DISABLE_AVX2=1` forces the scalar path in an
  // already-built binary. Lets the benchmark suite compare scalar and vector
  // kernels without a rebuild, and lets a bug report be bisected in the field.
  // getenv races only against setenv, and this runs once inside a
  // function-local static initialiser -- before any parsing thread exists.
  // Reading the environment is the only way to offer a kill switch in an
  // already-built binary.
  // NOLINTNEXTLINE(concurrency-mt-unsafe)
  if (const char* env = std::getenv("SIMDFIX_DISABLE_AVX2");
      env != nullptr && env[0] != '\0' && std::strcmp(env, "0") != 0) {
    return false;
  }
#if SIMDFIX_HAS_AVX2
#if defined(__GNUC__) && !defined(__clang__)
  __builtin_cpu_init();
#endif
  // GCC declares __builtin_cpu_supports as returning int, Clang as bool. The
  // explicit cast is what keeps this warning-free under GCC's -Wconversion; it
  // is genuinely redundant under Clang, hence the suppression rather than a
  // rewrite. Any spelling that satisfies one compiler's checks trips the
  // other's.
  // NOLINTNEXTLINE(readability-redundant-casting)
  return static_cast<bool>(__builtin_cpu_supports("avx2"));
#else
  return false;
#endif
}

}  // namespace impl

/// True when the AVX2 kernels are safe to execute on this CPU.
///
/// The result is cached in a function-local static, so the CPUID path runs
/// once. Reads after the first are a relaxed load of an initialised guard --
/// not free, but well below the cost of the branch it protects.
[[nodiscard]] inline bool has_avx2() noexcept {
  static const bool cached = impl::detect_avx2();
  return cached;
}

/// The ISA the dispatchers will select. Benchmarks report this so a pasted
/// result is never ambiguous about which kernel produced it.
[[nodiscard]] inline isa active_isa() noexcept {
  return has_avx2() ? isa::avx2 : isa::scalar;
}

[[nodiscard]] constexpr const char* to_string(isa v) noexcept {
  switch (v) {
    case isa::scalar:
      return "scalar";
    case isa::avx2:
      return "avx2";
  }
  return "unknown";
}

}  // namespace simdfix::detail

#endif  // SIMDFIX_DETAIL_CPU_HPP
