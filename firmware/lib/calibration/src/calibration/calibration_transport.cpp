#include "calibration/calibration_transport.h"

namespace sigap::calibration {
namespace {

bool criteria_equal(const CaptureCriteria& left,
                    const CaptureCriteria& right) noexcept {
  if (left.index() != right.index()) return false;
  return std::visit(
      [](const auto& lhs, const auto& rhs) noexcept {
        using L = std::decay_t<decltype(lhs)>;
        using R = std::decay_t<decltype(rhs)>;
        if constexpr (!std::is_same_v<L, R>) {
          return false;
        } else {
          if constexpr (std::is_same_v<L, Hx711CaptureCriteria>) {
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
        }
      },
      left, right);
}

bool read_only(CalibrationOperation operation) noexcept {
  return operation == CalibrationOperation::GetCapabilities ||
         operation == CalibrationOperation::GetStatus ||
         operation == CalibrationOperation::GetEvidence ||
         operation == CalibrationOperation::GetActiveProfile ||
         operation == CalibrationOperation::History;
}

}  // namespace

bool CalibrationTransportController::same_identity(
    const CalibrationCommandEnvelope& left,
    const CalibrationCommandEnvelope& right) noexcept {
  return left.request_id != 0U && left.request_id == right.request_id &&
         left.protocol_version == right.protocol_version &&
         left.device_id == right.device_id && left.session_id == right.session_id &&
         left.expires_at_us == right.expires_at_us &&
         left.transport == right.transport && left.operation == right.operation &&
         left.target == right.target && left.criteria_present == right.criteria_present &&
         (!left.criteria_present || criteria_equal(left.criteria, right.criteria)) &&
         left.anchor == right.anchor &&
         left.expected_profile_present == right.expected_profile_present &&
         (!left.expected_profile_present ||
          left.expected_profile.equals(right.expected_profile));
}

bool CalibrationTransportController::find_replay(
    const CalibrationCommandEnvelope& command,
    CalibrationResultEnvelope& output) const noexcept {
  for (const auto& entry : replay_) {
    if (entry.valid && entry.command.request_id == command.request_id) {
      if (!same_identity(entry.command, command)) {
        output = CalibrationResultEnvelope{};
        output.request_id = command.request_id;
        output.status = CalibrationCommandStatus::Conflict;
        return true;
      }
      output = entry.result;
      output.status = CalibrationCommandStatus::Replayed;
      return true;
    }
  }
  if (replay_store_ != nullptr) {
    CalibrationCommandEnvelope persisted_command{};
    CalibrationResultEnvelope persisted_result{};
    if (replay_store_->load(command.request_id, persisted_command,
                            persisted_result)) {
      if (!same_identity(persisted_command, command)) {
        output = CalibrationResultEnvelope{};
        output.request_id = command.request_id;
        output.status = CalibrationCommandStatus::Conflict;
        return true;
      }
      output = persisted_result;
      output.status = CalibrationCommandStatus::Replayed;
      return true;
    }
  }
  return false;
}

void CalibrationTransportController::remember(
    const CalibrationCommandEnvelope& command,
    const CalibrationResultEnvelope& result) noexcept {
  replay_[replay_next_].command = command;
  replay_[replay_next_].result = result;
  replay_[replay_next_].valid = true;
  replay_next_ = (replay_next_ + 1U) % replay_.size();
  if (replay_store_ != nullptr) {
    (void)replay_store_->save(command, result);
  }
}

void CalibrationTransportController::fill(
    const CalibrationCommandEnvelope& command,
    CalibrationResultEnvelope& output) const noexcept {
  output = CalibrationResultEnvelope{};
  output.protocol_version = command.protocol_version;
  output.request_id = command.request_id;
  output.session_id = session_id_;
  (void)workflow_.status(output.snapshot);
}

bool CalibrationTransportController::owns(
    const CalibrationCommandEnvelope& command) const noexcept {
  return owner_active_ && command.session_id == session_id_ &&
         command.transport == owner_;
}

void CalibrationTransportController::expire(uint64_t now_us) noexcept {
  if (!owner_active_) return;
  CalibrationStatusSnapshot snapshot{};
  if (!workflow_.status(snapshot)) return;
  if (snapshot.last_activity_us == 0U || now_us < snapshot.last_activity_us ||
      now_us - snapshot.last_activity_us <= CalibrationSession::kSessionTimeoutUs) {
    return;
  }
  (void)workflow_.cancel();
  owner_active_ = false;
  session_id_ = 0U;
}

bool CalibrationTransportController::expected_profile_matches(
    const CalibrationCommandEnvelope& command) const noexcept {
  if (!command.expected_profile_present) return true;
  CalibrationProfileRef active{};
  if (!workflow_.active_profile(command.target, active)) return false;
  return active.equals(command.expected_profile);
}

bool CalibrationTransportController::execute(
    const CalibrationCommandEnvelope& command, uint64_t now_us,
    CalibrationResultEnvelope& output) noexcept {
  expire(now_us);
  for (const auto& entry : replay_) {
    if (entry.valid && entry.command.request_id == command.request_id &&
        !same_identity(entry.command, command)) {
      fill(command, output);
      output.status = CalibrationCommandStatus::Conflict;
      return false;
    }
  }
  if (command.request_id != 0U && find_replay(command, output)) {
    return output.status == CalibrationCommandStatus::Replayed;
  }
  fill(command, output);
  if (command.protocol_version != CalibrationCommandEnvelope::kProtocolVersion ||
      command.device_id != device_id_ || command.request_id == 0U) {
    output.status = CalibrationCommandStatus::Invalid;
    return false;
  }
  if (command.expires_at_us != 0U && now_us > command.expires_at_us) {
    output.status = CalibrationCommandStatus::Expired;
    return false;
  }
  if (command.transport != CalibrationTransport::Usb &&
      (authorizer_ == nullptr || !authorizer_->authorize(command))) {
    output.status = CalibrationCommandStatus::Unauthorized;
    return false;
  }

  const bool is_start = command.operation == CalibrationOperation::Start;
  if (is_start && owner_active_ && command.transport != owner_) {
    output.status = CalibrationCommandStatus::SessionBusy;
    return false;
  }
  if (!read_only(command.operation) && !is_start && !owns(command)) {
    output.status = owner_active_ ? CalibrationCommandStatus::SessionBusy
                                  : CalibrationCommandStatus::Invalid;
    return false;
  }
  if (!read_only(command.operation) && !expected_profile_matches(command)) {
    output.status = CalibrationCommandStatus::Conflict;
    return false;
  }

  bool ok = false;
  switch (command.operation) {
    case CalibrationOperation::GetCapabilities:
      ok = true;
      break;
    case CalibrationOperation::GetStatus:
      ok = workflow_.status(output.snapshot);
      break;
    case CalibrationOperation::GetEvidence:
      ok = workflow_.evidence(command.target, output.evidence);
      break;
    case CalibrationOperation::GetActiveProfile:
      ok = workflow_.active_profile(command.target, output.profile);
      break;
    case CalibrationOperation::Start:
      ok = workflow_.start(command.target, now_us);
      if (ok) {
        owner_ = command.transport;
        owner_active_ = true;
        session_id_ = next_session_id_++;
        output.session_id = session_id_;
      }
      break;
    case CalibrationOperation::SetCriteria:
      ok = command.criteria_present && workflow_.set_criteria(command.criteria, now_us);
      break;
    case CalibrationOperation::Capture:
      ok = workflow_.capture(command.anchor, now_us);
      break;
    case CalibrationOperation::Validate:
      ok = workflow_.validate();
      break;
    case CalibrationOperation::Preview:
      ok = workflow_.preview(output.validation);
      output.preview_available = ok;
      break;
    case CalibrationOperation::Commit:
      ok = workflow_.commit();
      if (ok) {
        owner_active_ = false;
        session_id_ = 0U;
      }
      break;
    case CalibrationOperation::Cancel:
      ok = workflow_.cancel();
      if (ok) {
        owner_active_ = false;
        session_id_ = 0U;
      }
      break;
    case CalibrationOperation::History:
      ok = workflow_.history(output);
      break;
    case CalibrationOperation::Rollback:
      ok = workflow_.rollback();
      break;
  }
  output.status = ok ? CalibrationCommandStatus::Accepted
                     : CalibrationCommandStatus::Failed;
  if (command.request_id != 0U) remember(command, output);
  return ok;
}

void CalibrationTransportController::poll(uint64_t now_us) noexcept {
  expire(now_us);
}

void CalibrationTransportController::disconnect(
    CalibrationTransport transport) noexcept {
  if (owner_active_ && owner_ == transport) {
    (void)workflow_.cancel();
    owner_active_ = false;
    session_id_ = 0U;
  }
}

}  // namespace sigap::calibration
