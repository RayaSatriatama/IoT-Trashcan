#pragma once

#include <array>
#include <cstdint>

#include "calibration/calibration_session.h"

namespace sigap::calibration {

// The transport envelope deliberately carries references and criteria only.
// A capture request never carries a sensor reading; evidence is read from the
// instance-owned CalibrationEvidenceCache by the workflow implementation.
enum class CalibrationTransport : uint8_t { Usb, Ble, Https };

enum class CalibrationOperation : uint8_t {
  GetCapabilities,
  GetStatus,
  GetEvidence,
  GetActiveProfile,
  Start,
  SetCriteria,
  Capture,
  Validate,
  Preview,
  Commit,
  Cancel,
  History,
  Rollback,
};

enum class CalibrationCommandStatus : uint8_t {
  Accepted,
  Replayed,
  SessionBusy,
  Expired,
  Unauthorized,
  Invalid,
  Conflict,
  Failed,
};

struct CalibrationProfileRef final {
  bool present{false};
  uint64_t version{0U};
  std::array<uint8_t, 32U> hash{};

  bool equals(const CalibrationProfileRef& other) const noexcept {
    return present == other.present && version == other.version &&
           hash == other.hash;
  }
};

struct CalibrationStatusSnapshot final {
  CalibrationState state{CalibrationState::Idle};
  CalibrationTarget target{CalibrationTarget::Hx711Organic};
  CalibrationFailure failure{CalibrationFailure::None};
  CalibrationTransport owner{CalibrationTransport::Usb};
  bool active{false};
  uint64_t session_id{0U};
  uint32_t accepted_samples{0U};
  uint32_t rejected_samples{0U};
  uint64_t last_activity_us{0U};
};

struct CalibrationCommandEnvelope final {
  static constexpr uint16_t kProtocolVersion{1U};

  uint16_t protocol_version{kProtocolVersion};
  uint64_t request_id{0U};
  uint64_t device_id{0U};
  uint64_t session_id{0U};
  uint64_t expires_at_us{0U};
  CalibrationTransport transport{CalibrationTransport::Usb};
  CalibrationOperation operation{CalibrationOperation::GetStatus};
  CalibrationTarget target{CalibrationTarget::Hx711Organic};
  CaptureCriteria criteria{Hx711CaptureCriteria{}};
  bool criteria_present{false};
  CalibrationAnchor anchor{CalibrationAnchor::Primary};
  bool expected_profile_present{false};
  CalibrationProfileRef expected_profile{};
};

struct CalibrationResultEnvelope final {
  uint16_t protocol_version{CalibrationCommandEnvelope::kProtocolVersion};
  uint64_t request_id{0U};
  uint64_t session_id{0U};
  CalibrationCommandStatus status{CalibrationCommandStatus::Failed};
  CalibrationStatusSnapshot snapshot{};
  DiagnosticEvidence evidence{};
  CalibrationProfileRef profile{};
  ValidationResult validation{};
  bool preview_available{false};
};

class ICalibrationTransportAuthorizer {
 public:
  virtual ~ICalibrationTransportAuthorizer() = default;
  // BLE and HTTPS adapters must verify their encrypted/session-bound proof
  // before the controller accepts either reads or mutations. USB is already
  // gated by the local maintenance authorization boundary.
  virtual bool authorize(const CalibrationCommandEnvelope& command) const noexcept = 0;
};

class ICalibrationTransportWorkflow {
 public:
  virtual ~ICalibrationTransportWorkflow() = default;
  virtual bool status(CalibrationStatusSnapshot& output) const noexcept = 0;
  virtual bool evidence(CalibrationTarget target,
                        DiagnosticEvidence& output) const noexcept = 0;
  virtual bool active_profile(CalibrationTarget target,
                              CalibrationProfileRef& output) const noexcept = 0;
  virtual bool start(CalibrationTarget target, uint64_t now_us) noexcept = 0;
  virtual bool set_criteria(const CaptureCriteria& criteria,
                            uint64_t now_us) noexcept = 0;
  virtual bool capture(CalibrationAnchor anchor, uint64_t now_us) noexcept = 0;
  virtual bool validate() noexcept = 0;
  virtual bool preview(ValidationResult& output) const noexcept = 0;
  virtual bool commit() noexcept = 0;
  virtual bool cancel() noexcept = 0;
  virtual bool history(CalibrationResultEnvelope& output) const noexcept = 0;
  virtual bool rollback() noexcept = 0;
};

// A single controller is shared by USB, BLE and HTTPS adapters. It owns only
// arbitration/idempotency state; sensor evidence and profile persistence stay
// in the injected workflow and ProfileManager boundaries.
class CalibrationTransportController final {
 public:
  explicit CalibrationTransportController(
      ICalibrationTransportWorkflow& workflow,
      uint64_t device_id,
      const ICalibrationTransportAuthorizer* authorizer = nullptr) noexcept
      : workflow_(workflow), device_id_(device_id), authorizer_(authorizer) {}

  bool execute(const CalibrationCommandEnvelope& command,
               uint64_t now_us,
               CalibrationResultEnvelope& output) noexcept;
  void poll(uint64_t now_us) noexcept;
  void disconnect(CalibrationTransport transport) noexcept;
  bool active() const noexcept { return owner_active_; }
  CalibrationTransport owner() const noexcept { return owner_; }

 private:
  bool owns(const CalibrationCommandEnvelope& command) const noexcept;
  void expire(uint64_t now_us) noexcept;
  void fill(const CalibrationCommandEnvelope& command,
            CalibrationResultEnvelope& output) const noexcept;
  bool expected_profile_matches(
      const CalibrationCommandEnvelope& command) const noexcept;

  ICalibrationTransportWorkflow& workflow_;
  uint64_t device_id_{0U};
  const ICalibrationTransportAuthorizer* authorizer_{nullptr};
  CalibrationTransport owner_{CalibrationTransport::Usb};
  bool owner_active_{false};
  uint64_t session_id_{0U};
  uint64_t next_session_id_{1U};
  uint64_t last_request_id_{0U};
  CalibrationCommandEnvelope last_command_{};
  CalibrationResultEnvelope last_result_{};
  bool last_result_valid_{false};
};

}  // namespace sigap::calibration
