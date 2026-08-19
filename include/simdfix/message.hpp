// SPDX-License-Identifier: MIT
//
// A view over one framed FIX message, and a forward walk over its fields.
//
// Nothing here copies or allocates. `message_view` is two pointers wide and
// every `field` it yields borrows the caller's bytes.
#ifndef SIMDFIX_MESSAGE_HPP
#define SIMDFIX_MESSAGE_HPP

#include <cstddef>
#include <cstdint>
#include <iterator>
#include <optional>
#include <string_view>

#include "simdfix/checksum.hpp"
#include "simdfix/detail/cpu.hpp"
#include "simdfix/error.hpp"
#include "simdfix/field.hpp"
#include "simdfix/scan.hpp"

namespace simdfix {

/// Byte length of the FIX 4.2 trailer, `10=NNN<SOH>`. Fixed by the spec: the
/// checksum field is always exactly three digits.
inline constexpr std::size_t trailer_size = 7;

/// Forward walk over `tag=value<SOH>` pairs.
///
/// Iteration stops at the first malformed field rather than throwing; ask
/// `error()` afterwards to find out whether it stopped because the message
/// ended or because it was broken. A `message_view` obtained from `parse()` is
/// already structurally validated, so in that (normal) case `error()` is always
/// empty and the check can be skipped.
class field_iterator {
 public:
  // Deliberately *input*, not forward. The current field is stashed in the
  // iterator, so two equal iterators do not return references to the same
  // object -- which is precisely the multipass guarantee forward_iterator
  // makes. Claiming forward here would be a lie the standard library is
  // entitled to rely on.
  using iterator_concept = std::input_iterator_tag;
  using iterator_category = std::input_iterator_tag;
  using value_type = field;
  using difference_type = std::ptrdiff_t;
  using reference = const field&;
  using pointer = const field*;

  field_iterator() noexcept = default;

  explicit field_iterator(std::string_view fields SIMDFIX_LIFETIMEBOUND) noexcept : rest_(fields) {
    advance();
  }

  [[nodiscard]] reference operator*() const noexcept { return cur_; }

  [[nodiscard]] pointer operator->() const noexcept { return &cur_; }

  field_iterator& operator++() noexcept {
    advance();
    return *this;
  }

  field_iterator operator++(int) noexcept {
    field_iterator tmp = *this;
    advance();
    return tmp;
  }

  [[nodiscard]] friend bool operator==(const field_iterator& a, const field_iterator& b) noexcept {
    if (a.done_ || b.done_) {
      return a.done_ && b.done_;
    }
    return a.rest_.data() == b.rest_.data();
  }

  /// Set only when iteration stopped early because the bytes were malformed.
  [[nodiscard]] std::optional<parse_error> error() const noexcept { return error_; }

 private:
  void stop(std::optional<parse_error> err) noexcept {
    done_ = true;
    error_ = err;
    cur_ = {};
    rest_ = {};
  }

  void advance() noexcept {
    if (rest_.empty()) {
      stop(std::nullopt);
      return;
    }

    // Find the terminator first, then the `=` *within* the field.
    //
    // The other order looks equivalent and is not: searching for `=` across the
    // whole remaining buffer lets a field with no `=` silently borrow the next
    // field's, so `nonsense<SOH>10=000<SOH>` would be diagnosed as a bad tag
    // spanning a delimiter instead of a malformed field. Same rejection here,
    // but only because every such tag happens to contain a SOH; bounding the
    // search makes it true by construction rather than by luck.
    const std::size_t end = kernels::find_byte(rest_, soh);
    if (end == kernels::npos) {
      // Unterminated final field. Framing rules this out, so it is reachable
      // only through message_view::unchecked().
      stop(parse_error::malformed_field);
      return;
    }

    const std::string_view entry = rest_.substr(0, end);
    const std::size_t eq = kernels::find_byte(entry, equals);
    if (eq == kernels::npos || eq == 0) {
      // No `=` in the field, or an empty tag such as `=foo<SOH>`.
      stop(parse_error::malformed_field);
      return;
    }

    const auto tag = detail::parse_unsigned<std::uint32_t>(entry.substr(0, eq));
    if (!tag) {
      stop(parse_error::bad_tag);
      return;
    }

    cur_ = field{.tag = *tag, .value = entry.substr(eq + 1)};
    rest_ = rest_.substr(end + 1);
    done_ = false;
    error_ = std::nullopt;
  }

  std::string_view rest_{};
  field cur_{};
  std::optional<parse_error> error_{};
  bool done_ = true;
};

static_assert(std::input_iterator<field_iterator>);

/// A borrowed view of exactly one framed FIX message, `8=` through the trailing
/// `<SOH>` of `10=NNN`.
class message_view {
 public:
  message_view() = default;

  /// Wrap bytes that are *already known* to be a complete, correctly framed
  /// message. Named `unchecked` because it is: prefer `simdfix::parse()`, which
  /// establishes that precondition. This exists for tests, fuzzers, and callers
  /// whose transport already framed the message for them.
  [[nodiscard]] static message_view unchecked(std::string_view raw SIMDFIX_LIFETIMEBOUND) noexcept {
    message_view m;
    m.raw_ = raw;
    return m;
  }

  /// The whole message, trailer included.
  [[nodiscard]] std::string_view raw() const noexcept { return raw_; }

  [[nodiscard]] std::size_t size() const noexcept { return raw_.size(); }

  [[nodiscard]] bool empty() const noexcept { return raw_.empty(); }

  /// The bytes the checksum is computed over: everything up to but excluding
  /// `10=`. Exposed because "which bytes exactly?" is the question every FIX
  /// checksum bug turns out to be.
  [[nodiscard]] std::string_view checksum_region() const noexcept {
    return raw_.size() >= trailer_size ? raw_.substr(0, raw_.size() - trailer_size)
                                       : std::string_view{};
  }

  [[nodiscard]] field_iterator begin() const noexcept { return field_iterator{raw_}; }

  // The range concepts require end() to be callable as a non-static member on a
  // const object; that it happens not to read *this is an implementation detail
  // of the sentinel being default-constructed.
  // NOLINTNEXTLINE(readability-convert-member-functions-to-static)
  [[nodiscard]] field_iterator end() const noexcept { return {}; }

  /// First field with this tag, searched linearly from the front.
  ///
  /// Linear is the right default: FIX messages are short and a hash lookup does
  /// not pay for itself below a few dozen fields. Callers that need many
  /// lookups on one message should walk it once themselves.
  [[nodiscard]] result<field> find(std::uint32_t tag) const noexcept {
    field_iterator it = begin();
    const field_iterator last = end();
    for (; it != last; ++it) {
      if (it->tag == tag) {
        return *it;
      }
    }
    if (const auto err = it.error()) {
      return fail(*err);
    }
    return fail(parse_error::field_not_found);
  }

  /// Checksum recomputed from the bytes, via the dispatched kernel.
  [[nodiscard]] std::uint8_t computed_checksum() const noexcept {
    return kernels::checksum(checksum_region());
  }

  /// The value of tag 10 as stated in the message.
  [[nodiscard]] result<std::uint8_t> stated_checksum() const noexcept {
    if (raw_.size() < trailer_size) {
      return fail(parse_error::bad_checksum_field);
    }
    const std::string_view trailer = raw_.substr(raw_.size() - trailer_size);
    if (trailer.substr(0, 3) != "10=" || trailer.back() != soh) {
      return fail(parse_error::bad_checksum_field);
    }
    std::uint8_t value = 0;
    if (!kernels::parse_checksum(trailer.substr(3, 3), value)) {
      return fail(parse_error::bad_checksum_field);
    }
    return value;
  }

  [[nodiscard]] status validate_checksum() const noexcept {
    const auto stated = stated_checksum();
    if (!stated) {
      return fail(stated.error());
    }
    if (*stated != computed_checksum()) {
      return fail(parse_error::checksum_mismatch);
    }
    return {};
  }

  /// Walk every field, reporting the first structural problem.
  [[nodiscard]] status validate_fields() const noexcept {
    field_iterator it = begin();
    const field_iterator last = end();
    while (it != last) {
      ++it;
    }
    if (const auto err = it.error()) {
      return fail(*err);
    }
    return {};
  }

  /// Tag 35, the MsgType. `D` is NewOrderSingle, `8` is ExecutionReport.
  [[nodiscard]] result<std::string_view> msg_type() const noexcept {
    const auto f = find(tags::msg_type);
    if (!f) {
      return fail(f.error());
    }
    return f->value;
  }

  [[nodiscard]] result<std::uint64_t> msg_seq_num() const noexcept {
    const auto f = find(tags::msg_seq_num);
    if (!f) {
      return fail(f.error());
    }
    return f->as_uint();
  }

 private:
  std::string_view raw_{};
};

}  // namespace simdfix

#endif  // SIMDFIX_MESSAGE_HPP
