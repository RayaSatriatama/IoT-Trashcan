#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include "debug/io_gate.h"
#include "debug/service.h"
#include "network/network.h"
namespace sigap::debug {
struct DebugHttpResult {
  int status_code{};
  std::array<char, 512> ack_body{};
  size_t ack_size{};
  bool truncated{};
};
class IDebugHttpTransport {
 public:
  virtual ~IDebugHttpTransport() = default;
  // The owner preconfigures the transport with endpoint(); start does not take a URL.
  // Views remain alive until quiescent(). Response means poll can collect it now,
  // independently of cleanup; Pending means completion has not yet been observed.
  virtual network::TransportResult start(std::string_view exact_body,
                                         std::string_view token,
                                         uint32_t timeout_ms) noexcept = 0;
  virtual network::TransportResult poll(DebugHttpResult&) noexcept = 0;
  virtual void cancel() noexcept = 0;
  virtual bool quiescent() const noexcept = 0;
};
struct DebugReadiness {
  bool wifi{}, time_valid{}, ca_ready{}, config_ready{};
  bool control_active{}, other_transport_active{};
};
enum class DebugWorkerStatus : uint8_t {
  Unconfigured, Idle, Waiting, Delivered, RetryScheduled, Unavailable,
  Stopped, DeliveryFailure, ProtocolError
};
class DebugHttpWorker final {
 public:
  DebugHttpWorker(DebugService&, IDebugHttpTransport&, RuntimeIoGate&, std::string_view device_id) noexcept;
  DebugHttpWorker(const DebugHttpWorker&) = delete;
  DebugHttpWorker& operator=(const DebugHttpWorker&) = delete;
  // Copies bounded config; rejects changes while transport borrows buffers.
  // Identical config preserves sticky stops; changed validated config restarts
  // the budget for the retained snapshot. No live transport/TLS is configured here.
  bool configure(std::string_view endpoint, std::string_view token) noexcept;
  void poll(uint64_t now_us, const DebugReadiness&) noexcept;
  void cancel() noexcept;
  std::string_view endpoint() const noexcept { return {endpoint_.data(), endpoint_size_}; }
  bool transport_pending() const noexcept { return owns_transport_; }
  DebugWorkerStatus status() const noexcept { return status_; }
  // Permanently undelivered event records, not failed attempts or batches.
  uint64_t delivery_loss() const noexcept { return delivery_loss_; }
  uint64_t cursor() const noexcept { return cursor_.after; }
 private:
  bool prepare(uint64_t now_us) noexcept;
  void poll_active(uint64_t now_us, const DebugReadiness&) noexcept;
  void finish_attempt(uint64_t now_us) noexcept;
  void release_if_quiescent() noexcept;
  void fail_attempt(uint64_t now_us) noexcept;
  DebugService& service_;
  IDebugHttpTransport& transport_;
  RuntimeIoGate& gate_;
  std::array<char, 65> device_id_{};
  size_t device_size_{};
  std::array<char, 193> endpoint_{};
  size_t endpoint_size_{};
  std::array<char, 129> token_{};
  size_t token_size_{};
  BatchSnapshot frozen_{};
  std::array<char, 4096> body_{};
  size_t body_size_{};
  std::array<uint8_t, 32> digest_{};
  Cursor cursor_{};
  uint64_t next_start_us_{}, deadline_us_{}, retry_at_us_{}, delivery_loss_{};
  uint8_t attempts_{};
  bool configured_{}, owns_transport_{}, canceling_{}, sticky_stop_{}, response_ready_{};
  network::TransportResult completed_result_{network::TransportResult::Pending};
  DebugHttpResult completed_{};
  DebugWorkerStatus status_{DebugWorkerStatus::Unconfigured};
};
} // namespace sigap::debug
