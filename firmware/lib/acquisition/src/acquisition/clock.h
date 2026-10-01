#pragma once

#include <cstdint>

namespace sigap::acquisition {

// Small injectable clock seam for native tests and host-side composition.
// The supplied reader must return a non-decreasing value in microseconds.
class InjectedMonotonicClock final {
 public:
  using Reader = uint64_t (*)(void*) noexcept;

  constexpr InjectedMonotonicClock(Reader reader, void* context) noexcept
      : reader_(reader), context_(context) {}

  uint64_t now_us() const noexcept {
    return reader_ == nullptr ? 0U : reader_(context_);
  }

 private:
  Reader reader_{nullptr};
  void* context_{nullptr};
};

}  // namespace sigap::acquisition
