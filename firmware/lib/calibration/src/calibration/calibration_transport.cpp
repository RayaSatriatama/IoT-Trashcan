#include "calibration/calibration_transport.h"

namespace sigap::calibration {
namespace {

bool same_request(const CalibrationCommandEnvelope& left,
                 const CalibrationCommandEnvelope& right) noexcept {
  return left.request_id != 0U && left.request_id == right.request_id &&
         left.device_id == right.device_id &&
         left.operation == right.operation && left.session_id == right.session_id;
}

bool read_only(CalibrationOperation operation) noexcept {
  return operation == CalibrationOperation::GetCapabilities ||
         operation == CalibrationOperation::GetStatus ||
         operation == CalibrationOperation::GetEvidence ||
         operation == CalibrationOperation::GetActiveProfile ||
         operation == CalibrationOperation::History;
}

}  // namespace

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
  if (last_result_valid_ && same_request(command, last_command_)) {
    output = last_result_;
    output.status = CalibrationCommandStatus::Replayed;
    return true;
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
  if (command.request_id != 0U) {
    last_command_ = command;
    last_result_ = output;
    last_result_valid_ = true;
  }
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
