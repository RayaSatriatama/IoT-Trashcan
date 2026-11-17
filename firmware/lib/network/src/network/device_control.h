#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

#include "network/network.h"

namespace sigap::network {

// The device-control wire payload is parsed by the transport boundary into
// these bounded fields.  Firmware never accepts an unbounded JSON document or
// a caller-supplied raw sensor value as part of a command.
inline constexpr std::size_t kMaxDeviceControlIdBytes{64U};
inline constexpr std::size_t kMaxDeviceControlSensorBytes{32U};
inline constexpr std::size_t kMaxDeviceControlOperationBytes{32U};
inline constexpr std::size_t kMaxDeviceControlPayloadBytes{4096U};
inline constexpr std::size_t kMaxDeviceControlRequestHashBytes{71U};
inline constexpr std::size_t kMaxDeviceControlErrorBytes{64U};
// The server supplies this relative lease alongside its wall-clock expiry.
// A device without a trusted UTC anchor must not derive a deadline from an
// RFC3339 timestamp alone.
inline constexpr uint32_t kMaxDeviceControlExpiryMs{120000U};

inline bool device_control_deadline_from_relative_ms(
    uint64_t now_us, uint32_t expires_in_ms, uint64_t& deadline_us) noexcept {
  if (expires_in_ms == 0U || expires_in_ms > kMaxDeviceControlExpiryMs ||
      expires_in_ms > (UINT64_MAX - now_us) / 1000U) {
    deadline_us = 0U;
    return false;
  }
  deadline_us = now_us + static_cast<uint64_t>(expires_in_ms) * 1000U;
  return true;
}

template <std::size_t N>
struct DeviceControlText final {
  std::array<char, N + 1U> bytes{};
  std::size_t size{0U};

  bool assign(std::string_view value) noexcept {
    if (value.empty() || value.size() > N) return false;
    std::copy(value.begin(), value.end(), bytes.begin());
    bytes[value.size()] = '\0';
    size = value.size();
    return true;
  }
  std::string_view view() const noexcept {
    return std::string_view(bytes.data(), size);
  }
};

enum class DeviceControlStatus : uint8_t {
  Accepted,
  Capturing,
  Validated,
  PreviewReady,
  Committed,
  Failed,
  Cancelled,
};

struct DeviceControlCommand final {
  DeviceControlText<kMaxDeviceControlIdBytes> command_id{};
  DeviceControlText<kMaxDeviceControlIdBytes> device_id{};
  DeviceControlText<kMaxDeviceControlIdBytes> operator_id{};
  DeviceControlText<kMaxDeviceControlIdBytes> session_id{};
  DeviceControlText<kMaxDeviceControlSensorBytes> sensor_id{};
  DeviceControlText<kMaxDeviceControlOperationBytes> operation{};
  DeviceControlText<kMaxDeviceControlRequestHashBytes> request_hash{};
  std::array<uint8_t, kMaxDeviceControlPayloadBytes> payload{};
  std::size_t payload_size{0U};
  uint64_t expires_at_us{0U};
  uint32_t attempt{0U};
};

struct DeviceControlResult final {
  DeviceControlStatus status{DeviceControlStatus::Failed};
  DeviceControlText<kMaxDeviceControlIdBytes> command_id{};
  DeviceControlText<kMaxDeviceControlIdBytes> session_id{};
  std::array<uint8_t, kMaxDeviceControlPayloadBytes> payload{};
  std::size_t payload_size{0U};
  DeviceControlText<kMaxDeviceControlErrorBytes> error_code{};
};

struct DeviceControlHttpResponse final {
  int status_code{0};
  bool command_present{false};
  DeviceControlCommand command{};
};

class IDeviceControlTransport {
 public:
  virtual ~IDeviceControlTransport() = default;
  virtual TransportResult start_next_command(
      std::string_view device_id, std::string_view token, uint32_t timeout_ms,
      DeviceControlHttpResponse& response) noexcept = 0;
  virtual TransportResult poll_next_command(
      DeviceControlHttpResponse& response) noexcept = 0;
  virtual TransportResult start_command_result(
      std::string_view device_id, std::string_view token,
      const DeviceControlCommand& command, const DeviceControlResult& result,
      uint32_t timeout_ms, DeviceControlHttpResponse& response) noexcept = 0;
  virtual TransportResult poll_command_result(
      DeviceControlHttpResponse& response) noexcept = 0;
  virtual void cancel_command_request() noexcept = 0;
};

class IDeviceControlExecutor {
 public:
  virtual ~IDeviceControlExecutor() = default;
  // The executor is the only component allowed to mutate calibration state.
  // It must implement request idempotency and profile readback for COMMIT.
  virtual bool execute(const DeviceControlCommand& command,
                       uint64_t now_us,
                       DeviceControlResult& result) noexcept = 0;
};

class IDeviceControlResultJournal {
 public:
  virtual ~IDeviceControlResultJournal() = default;
  // Startup recovery is separate from loading a record.  A missing journal
  // is healthy and returns true; corrupt or inaccessible storage returns
  // false so the poller cannot fetch a new mutating command.
  virtual bool recover() noexcept { return true; }
  virtual bool load(DeviceControlCommand& command,
                    DeviceControlResult& result) const noexcept = 0;
  virtual bool save(const DeviceControlCommand& command,
                    const DeviceControlResult& result) noexcept = 0;
  virtual bool clear() noexcept = 0;
};

enum class DeviceControlPollOutcome : uint8_t {
  Idle,
  Waiting,
  Executed,
  ResultSent,
  Backoff,
  Unauthorized,
  Expired,
  Invalid,
  Failed,
};

class DeviceControlPoller final {
 public:
  DeviceControlPoller(const NetworkPolicy& policy, IMonotonicClock& clock,
                      IDeviceControlTransport& transport,
                      IDeviceControlExecutor& executor,
                      IDeviceControlResultJournal* journal = nullptr) noexcept
      : policy_(policy), clock_(clock), transport_(transport),
        executor_(executor), journal_(journal) {}

  bool begin(std::string_view device_id, std::string_view token) noexcept {
    if (!policy_.valid() || !device_id_text_.assign(device_id) ||
        token.size() == 0U || token.size() > 128U) {
      state_ = State::AuthRequired;
      return false;
    }
    std::copy(token.begin(), token.end(), token_.begin());
    token_size_ = token.size();
    retry_attempt_ = 0U;
    next_attempt_us_ = 0U;
    command_ = DeviceControlCommand{};
    result_ = DeviceControlResult{};
    result_request_pending_ = false;
    if (journal_ != nullptr && !journal_->recover()) {
      state_ = State::JournalUnavailable;
      return false;
    }
    if (journal_ != nullptr && journal_->load(command_, result_)) {
      if (!valid_recovered_result(command_, result_)) {
        if (!journal_->clear()) {
          state_ = State::JournalUnavailable;
          return false;
        }
      } else {
        state_ = State::PostingResult;
        return true;
      }
    }
    state_ = State::Idle;
    return true;
  }

  void authorize_again() noexcept {
    if (state_ == State::AuthRequired) state_ = State::Idle;
  }

  DeviceControlPollOutcome poll() noexcept {
    const uint64_t now = clock_.now_us();
    if (state_ == State::AuthRequired) return DeviceControlPollOutcome::Unauthorized;
    if (state_ == State::JournalUnavailable) return DeviceControlPollOutcome::Failed;
    if (now < next_attempt_us_) return DeviceControlPollOutcome::Backoff;

    if (state_ == State::PersistingResult) return persist_result(now);
    if (state_ == State::PostingResult) return poll_result(now);
    if (state_ == State::ClearingJournal) return clear_result(now);
    if (state_ == State::WaitingCommand) {
      DeviceControlHttpResponse response{};
      const auto transport_result = transport_.poll_next_command(response);
      if (transport_result == TransportResult::Pending) {
        return DeviceControlPollOutcome::Waiting;
      }
      state_ = State::Idle;
      return handle_command_response(transport_result, response, now);
    }

    DeviceControlHttpResponse response{};
    const auto transport_result = transport_.start_next_command(
        device_id_text_.view(), std::string_view(token_.data(), token_size_),
        policy_.request_timeout_ms, response);
    if (transport_result == TransportResult::Pending) {
      state_ = State::WaitingCommand;
      return DeviceControlPollOutcome::Waiting;
    }
    return handle_command_response(transport_result, response, now);
  }

  bool busy() const noexcept {
    return state_ == State::WaitingCommand || state_ == State::PersistingResult ||
           state_ == State::PostingResult || state_ == State::ClearingJournal ||
           state_ == State::JournalUnavailable;
  }
  NetworkState state() const noexcept {
    return state_ == State::AuthRequired ? NetworkState::AuthRequired
                                         : state_ == State::JournalUnavailable
                                               ? NetworkState::Offline
                                         : state_ == State::Idle
                                               ? NetworkState::Offline
                                               : NetworkState::Online;
  }
  uint64_t next_attempt_us() const noexcept { return next_attempt_us_; }
  const DeviceControlCommand& command() const noexcept { return command_; }
  const DeviceControlResult& result() const noexcept { return result_; }

 private:
  enum class State : uint8_t {
    Idle,
    WaitingCommand,
    PersistingResult,
    PostingResult,
    ClearingJournal,
    AuthRequired,
    JournalUnavailable,
  };

  bool valid_command(const DeviceControlCommand& command,
                    uint64_t now_us) const noexcept {
    return command.command_id.size != 0U && command.device_id.view() == device_id_text_.view() &&
           command.operator_id.size != 0U && command.request_hash.size != 0U &&
           command.sensor_id.size != 0U && command.operation.size != 0U &&
           command.payload_size <= command.payload.size() &&
           command.expires_at_us != 0U && now_us <= command.expires_at_us;
  }

  static bool requires_session(std::string_view operation) noexcept {
    return operation == "set_criteria" || operation == "criteria" ||
           operation == "capture" || operation == "validate" ||
           operation == "preview" || operation == "commit" ||
           operation == "cancel" || operation == "rollback";
  }

  bool valid_recovered_result(const DeviceControlCommand& command,
                              const DeviceControlResult& result) const noexcept {
    // expires_at_us is relative to the uptime of the boot that accepted the
    // command. It cannot authorize a new execution after reboot, but it also
    // must not discard an already executed result awaiting reconciliation.
    return command.command_id.size != 0U &&
           command.device_id.view() == device_id_text_.view() &&
           command.operator_id.size != 0U &&
           (!requires_session(command.operation.view()) ||
            command.session_id.size != 0U) &&
           command.request_hash.size != 0U && command.sensor_id.size != 0U &&
           command.operation.size != 0U &&
           command.payload_size <= command.payload.size() &&
           result.command_id.view() == command.command_id.view() &&
           result.session_id.view() == command.session_id.view() &&
           result.payload_size != 0U &&
           result.payload_size <= result.payload.size();
  }

  void schedule_retry(uint64_t now_us) noexcept {
    uint64_t delay = policy_.retry_min_ms;
    const uint32_t shifts = std::min<uint32_t>(retry_attempt_, 31U);
    delay = std::min<uint64_t>(delay << shifts, policy_.retry_max_ms);
    next_attempt_us_ = now_us + delay * 1000U;
    if (retry_attempt_ != UINT32_MAX) ++retry_attempt_;
  }

  DeviceControlPollOutcome handle_command_response(
      TransportResult transport_result, const DeviceControlHttpResponse& response,
      uint64_t now_us) noexcept {
    if (transport_result == TransportResult::Pending) return DeviceControlPollOutcome::Waiting;
    if (transport_result == TransportResult::Timeout ||
        transport_result == TransportResult::Failed) {
      schedule_retry(now_us);
      return DeviceControlPollOutcome::Failed;
    }
    if (response.status_code == 401 || response.status_code == 403) {
      state_ = State::AuthRequired;
      return DeviceControlPollOutcome::Unauthorized;
    }
    if (response.status_code == 204 || !response.command_present) {
      schedule_retry(now_us);
      return DeviceControlPollOutcome::Idle;
    }
    if (response.status_code != 200 || !valid_command(response.command, now_us)) {
      schedule_retry(now_us);
      return response.command_present ? DeviceControlPollOutcome::Invalid
                                      : DeviceControlPollOutcome::Failed;
    }
    command_ = response.command;
    result_ = DeviceControlResult{};
    const bool session_required = requires_session(command_.operation.view());
    if (!result_.command_id.assign(command_.command_id.view()) ||
        (session_required && command_.session_id.size == 0U) ||
        (command_.session_id.size != 0U &&
         !result_.session_id.assign(command_.session_id.view()))) {
      schedule_retry(now_us);
      return DeviceControlPollOutcome::Invalid;
    }
    const bool executed = executor_.execute(command_, now_us, result_);
    if (!executed && result_.status != DeviceControlStatus::Failed) {
      result_.status = DeviceControlStatus::Failed;
    }
    if (journal_ != nullptr && !journal_->save(command_, result_)) {
      state_ = State::PersistingResult;
      schedule_retry(now_us);
      return DeviceControlPollOutcome::Failed;
    }
    state_ = State::PostingResult;
    return DeviceControlPollOutcome::Executed;
  }

  DeviceControlPollOutcome persist_result(uint64_t now_us) noexcept {
    if (journal_ == nullptr || !journal_->save(command_, result_)) {
      schedule_retry(now_us);
      return DeviceControlPollOutcome::Failed;
    }
    state_ = State::PostingResult;
    retry_attempt_ = 0U;
    next_attempt_us_ = now_us;
    return DeviceControlPollOutcome::Executed;
  }

  DeviceControlPollOutcome poll_result(uint64_t now_us) noexcept {
    DeviceControlHttpResponse response{};
    const auto transport_result = result_request_pending_
                                      ? transport_.poll_command_result(response)
                                      : transport_.start_command_result(
                                            device_id_text_.view(),
                                            std::string_view(token_.data(), token_size_),
                                            command_, result_,
                                            policy_.request_timeout_ms, response);
    if (transport_result == TransportResult::Pending) {
      result_request_pending_ = true;
      return DeviceControlPollOutcome::Waiting;
    }
    result_request_pending_ = false;
    if (transport_result == TransportResult::Timeout ||
        transport_result == TransportResult::Failed) {
      schedule_retry(now_us);
      return DeviceControlPollOutcome::Failed;
    }
    if (response.status_code == 401 || response.status_code == 403) {
      state_ = State::AuthRequired;
      return DeviceControlPollOutcome::Unauthorized;
    }
    if (response.status_code < 200 || response.status_code >= 300) {
      schedule_retry(now_us);
      return DeviceControlPollOutcome::Failed;
    }
    if (journal_ != nullptr && !journal_->clear()) {
      state_ = State::ClearingJournal;
      schedule_retry(now_us);
      return DeviceControlPollOutcome::Failed;
    }
    state_ = State::Idle;
    retry_attempt_ = 0U;
    next_attempt_us_ = now_us;
    return DeviceControlPollOutcome::ResultSent;
  }

  DeviceControlPollOutcome clear_result(uint64_t now_us) noexcept {
    if (journal_ == nullptr || !journal_->clear()) {
      schedule_retry(now_us);
      return DeviceControlPollOutcome::Failed;
    }
    state_ = State::Idle;
    retry_attempt_ = 0U;
    next_attempt_us_ = now_us;
    command_ = DeviceControlCommand{};
    result_ = DeviceControlResult{};
    return DeviceControlPollOutcome::ResultSent;
  }

  NetworkPolicy policy_{};
  IMonotonicClock& clock_;
  IDeviceControlTransport& transport_;
  IDeviceControlExecutor& executor_;
  IDeviceControlResultJournal* journal_{nullptr};
  DeviceControlText<kMaxDeviceControlIdBytes> device_id_text_{};
  std::array<char, 129U> token_{};
  std::size_t token_size_{0U};
  DeviceControlCommand command_{};
  DeviceControlResult result_{};
  State state_{State::Idle};
  uint32_t retry_attempt_{0U};
  uint64_t next_attempt_us_{0U};
  bool result_request_pending_{false};
};

}  // namespace sigap::network
