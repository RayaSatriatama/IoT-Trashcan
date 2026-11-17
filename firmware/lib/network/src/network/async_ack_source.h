#pragma once

#include <cstdint>
#include "network/network.h"

#ifdef ARDUINO
namespace sigap::network {

struct AckWorkerSnapshot final {
  bool valid{false};
  bool ready{false};
  bool busy{true};
  int32_t main_core{-1};
  int32_t worker_core{-1};
  uint32_t started{0U};
  uint32_t completed{0U};
  uint32_t stack_hwm{0U};  // Raw installed-FreeRTOS units; not verified bytes.
  uint64_t last_duration_us{0U};
  DeliveryStatus last_status{DeliveryStatus::Retryable};
};

class AsyncAckBatchSource final : public IImmutableBatchSource {
 public:
  explicit AsyncAckBatchSource(IImmutableBatchSource& inner) noexcept;
  ~AsyncAckBatchSource() override;
  AsyncAckBatchSource(const AsyncAckBatchSource&) = delete;
  AsyncAckBatchSource& operator=(const AsyncAckBatchSource&) = delete;
  // Success means task creation accepted; ready() separately confirms its core.
  bool begin(uint32_t main_core) noexcept;
  bool ready() const noexcept;
  bool busy() const noexcept;
  bool peek_pending(const telemetry_archive::ImmutableBatch*& batch) noexcept override;
  DeliveryStatus acknowledge(const telemetry_archive::Ack& ack) noexcept override;
  AckWorkerSnapshot snapshot() const noexcept;
 private:
  struct State;
  static void task_entry(void* context) noexcept;
  IImmutableBatchSource& inner_;
  State* state_{nullptr};
};

}  // namespace sigap::network
#endif
