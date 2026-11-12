#pragma once

#include <array>
#include <cstdint>
#include <cstddef>
#include <type_traits>

#include "calibration/calibration_session.h"
#include "debug/types.h"

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
  Committed,
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
  // Stable transport principal identity used by replay/arbitration metadata.
  // It is not proof of operator authentication for the BLE prototype.
  uint64_t principal_id{0U};
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
  // Digest of the complete canonical command payload. The replay ring keeps
  // the digest and size rather than an unbounded JSON document.
  std::array<uint8_t, 32U> payload_digest{};
  uint16_t payload_size{0U};
};

// Replay identity is stable across delivery attempts and reboot.  The
// expires_at_us field is a local execution deadline and is intentionally not
// part of this comparison.
inline bool calibration_command_identity_equal(
    const CalibrationCommandEnvelope& left,
    const CalibrationCommandEnvelope& right) noexcept {
  if (left.request_id == 0U || left.request_id != right.request_id ||
      left.protocol_version != right.protocol_version ||
      left.device_id != right.device_id ||
      left.principal_id != right.principal_id ||
      left.session_id != right.session_id ||
      left.transport != right.transport ||
      left.operation != right.operation ||
      left.target != right.target ||
      left.criteria_present != right.criteria_present ||
      left.anchor != right.anchor ||
      left.expected_profile_present != right.expected_profile_present ||
      left.payload_size != right.payload_size ||
      left.payload_digest != right.payload_digest) {
    return false;
  }
  if (left.expected_profile_present &&
      !left.expected_profile.equals(right.expected_profile)) {
    return false;
  }
  if (!left.criteria_present || left.criteria.index() != right.criteria.index()) {
    if (left.criteria_present) return false;
  } else {
    const bool equal = std::visit(
        [](const auto& lhs, const auto& rhs) noexcept {
          using L = std::decay_t<decltype(lhs)>;
          using R = std::decay_t<decltype(rhs)>;
          if constexpr (!std::is_same_v<L, R>) {
            return false;
          } else if constexpr (std::is_same_v<L, Hx711CaptureCriteria>) {
            return lhs.required_samples == rhs.required_samples &&
                   lhs.max_span_counts == rhs.max_span_counts;
          } else if constexpr (std::is_same_v<L, DistanceCaptureCriteria>) {
            return lhs.required_samples == rhs.required_samples &&
                   lhs.max_span_mm == rhs.max_span_mm;
          } else if constexpr (std::is_same_v<L, MqCaptureCriteria>) {
            return lhs.required_samples == rhs.required_samples &&
                   lhs.max_span_counts == rhs.max_span_counts &&
                   lhs.require_low_detect == rhs.require_low_detect;
          } else if constexpr (std::is_same_v<L, Ds18b20CaptureCriteria>) {
            return lhs.required_samples == rhs.required_samples &&
                   lhs.max_span_centi_c == rhs.max_span_centi_c &&
                   lhs.require_rom_match == rhs.require_rom_match;
          } else {
            return lhs.required_samples == rhs.required_samples &&
                   lhs.max_temperature_span_centi_c ==
                       rhs.max_temperature_span_centi_c &&
                   lhs.max_humidity_span_centi_pct ==
                       rhs.max_humidity_span_centi_pct;
          }
        },
        left.criteria, right.criteria);
    if (!equal) return false;
  }
  return true;
}

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

// Optional durable replay boundary.  A target may back this with the same
// authenticated NVS/profile journal used by ProfileManager.  The controller
// remains safe when no store is installed: retries are still compared against
// the bounded in-memory replay ring, while a persistent implementation can
// recover a lost COMMIT response after reboot.
class ICalibrationTransportReplayStore {
 public:
  virtual ~ICalibrationTransportReplayStore() = default;
  // COMMIT writes a durable intent before profile activation.  If the
  // response is lost after activation, the intent lets a later retry verify
  // the active profile without running the side effect again.
  virtual bool prepare(const CalibrationCommandEnvelope&) noexcept {
    return true;
  }
  virtual bool load(uint64_t request_id,
                    CalibrationCommandEnvelope& command,
                    CalibrationResultEnvelope& result) const noexcept = 0;
  virtual bool load_prepared(
      uint64_t request_id,
      CalibrationCommandEnvelope& command) const noexcept {
    (void)request_id;
    (void)command;
    return false;
  }
  virtual bool save(const CalibrationCommandEnvelope& command,
                    const CalibrationResultEnvelope& result) noexcept = 0;
};

class ICalibrationTransportWorkflow {
 public:
  virtual ~ICalibrationTransportWorkflow() = default;
  virtual bool status(CalibrationStatusSnapshot& output) const noexcept = 0;
  virtual bool evidence(CalibrationTarget target,
                        DiagnosticEvidence& output) const noexcept = 0;
  virtual bool active_profile(CalibrationTarget target,
                              CalibrationProfileRef& output) const noexcept = 0;
  // During PREVIEW the workflow may expose the exact profile identity that a
  // subsequent COMMIT is allowed to activate. Implementations that do not
  // expose a proposal can return false; COMMIT still requires their normal
  // lifecycle validation.
  virtual bool proposed_profile(
      CalibrationProfileRef& output) const noexcept {
    output = CalibrationProfileRef{};
    return false;
  }
  virtual bool start(CalibrationTarget target, uint64_t now_us) noexcept = 0;
  virtual bool set_criteria(const CaptureCriteria& criteria,
                            uint64_t now_us) noexcept = 0;
  virtual bool capture(CalibrationAnchor anchor, uint64_t now_us) noexcept = 0;
  virtual bool validate() noexcept = 0;
  virtual bool preview(ValidationResult& output) noexcept = 0;
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
      const ICalibrationTransportAuthorizer* authorizer = nullptr,
      ICalibrationTransportReplayStore* replay_store = nullptr) noexcept
      : workflow_(workflow), device_id_(device_id), authorizer_(authorizer),
        replay_store_(replay_store) {}

  bool execute(const CalibrationCommandEnvelope& command,
               uint64_t now_us,
               CalibrationResultEnvelope& output) noexcept;
  void poll(uint64_t now_us) noexcept;
  void disconnect(CalibrationTransport transport) noexcept;
  void disconnect(CalibrationTransport transport, uint64_t now_us) noexcept;
  void set_device_id(uint64_t device_id) noexcept { device_id_ = device_id; }
  bool active() const noexcept { return owner_active_; }
  CalibrationTransport owner() const noexcept { return owner_; }
  uint64_t device_id() const noexcept { return device_id_; }
  void set_debug_sink(debug::IEventSink* sink) noexcept { debug_sink_ = sink; }

 private:
  bool owns(const CalibrationCommandEnvelope& command) const noexcept;
  void expire(uint64_t now_us) noexcept;
  void emit_result(const CalibrationCommandEnvelope& command,
                   CalibrationCommandStatus status, uint64_t now_us,
                   const CalibrationProfileRef* profile = nullptr) noexcept;
  void emit_owner(uint64_t now_us, bool active,
                  CalibrationTransport owner) noexcept;
  void fill(const CalibrationCommandEnvelope& command,
            CalibrationResultEnvelope& output) const noexcept;
  bool expected_profile_matches(
      const CalibrationCommandEnvelope& command) const noexcept;

  static bool same_identity(const CalibrationCommandEnvelope& left,
                            const CalibrationCommandEnvelope& right) noexcept;
  bool find_replay(const CalibrationCommandEnvelope& command,
                   CalibrationResultEnvelope& output) noexcept;
  bool remember(const CalibrationCommandEnvelope& command,
                const CalibrationResultEnvelope& result) noexcept;

  static constexpr std::size_t kReplayEntries{4U};
  struct ReplayEntry final {
    CalibrationCommandEnvelope command{};
    CalibrationResultEnvelope result{};
    bool valid{false};
  };

  ICalibrationTransportWorkflow& workflow_;
  uint64_t device_id_{0U};
  const ICalibrationTransportAuthorizer* authorizer_{nullptr};
  ICalibrationTransportReplayStore* replay_store_{nullptr};
  debug::IEventSink* debug_sink_{nullptr};
  CalibrationTransport owner_{CalibrationTransport::Usb};
  bool owner_active_{false};
  uint64_t session_id_{0U};
  uint64_t principal_id_{0U};
  uint64_t next_session_id_{1U};
  std::array<ReplayEntry, kReplayEntries> replay_{};
  std::size_t replay_next_{0U};
};

}  // namespace sigap::calibration
