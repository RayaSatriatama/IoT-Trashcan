#pragma once

#include <cstddef>
#include <cstdint>

#include "acquisition/record.h"
#include "debug/types.h"

namespace sigap {

// Emits coalesced typed acquisition-quality transitions; cache is per observer.
class RuntimeDebugObserver final : public acquisition::ISampleObserver {
 public:
  RuntimeDebugObserver(debug::IEventSink& sink,
                       acquisition::ISampleObserver& evidence) noexcept;
  void observe(const acquisition::SampleRecord& sample) noexcept override;

 private:
  struct State {
    uint8_t quality{0U};
    uint8_t reason{0U};
    bool present{false};
  };
  static constexpr std::size_t kSensorCount{13U};

  debug::IEventSink& sink_;
  acquisition::ISampleObserver& evidence_;
  std::array<State, kSensorCount> states_{};
};

}  // namespace sigap
