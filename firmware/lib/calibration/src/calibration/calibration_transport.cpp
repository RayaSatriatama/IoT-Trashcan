#include "calibration/calibration_transport.h"
#include "sigap/build/build_config.h"

namespace sigap::calibration {
namespace {

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
  return calibration_command_identity_equal(left, right);
}

bool CalibrationTransportController::find_replay(
    const CalibrationCommandEnvelope& command,
    CalibrationResultEnvelope& output) noexcept {
  for (const auto& entry : replay_) {
    if (entry.valid && entry.command.request_id == command.request_id) {
      if (!same_identity(entry.command, command)) {
        output = CalibrationResultEnvelope{};
        output.request_id = command.request_id;
        output.status = CalibrationCommandStatus::Conflict;
        return true;
      }
      output = entry.result;
      if (output.status == CalibrationCommandStatus::Accepted ||
          output.status == CalibrationCommandStatus::Committed)
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
      if (output.status == CalibrationCommandStatus::Accepted ||
          output.status == CalibrationCommandStatus::Committed)
        output.status = CalibrationCommandStatus::Replayed;
      return true;
    }
    CalibrationCommandEnvelope prepared_command{};
    if (replay_store_->load_prepared(command.request_id, prepared_command)) {
      if (!same_identity(prepared_command, command)) {
        output = CalibrationResultEnvelope{};
        output.request_id = command.request_id;
        output.status = CalibrationCommandStatus::Conflict;
        return true;
      }
      CalibrationProfileRef active{};
      if (command.operation != CalibrationOperation::Commit ||
          !command.expected_profile_present ||
          !workflow_.active_profile(command.target, active) ||
          !active.present || !active.equals(command.expected_profile)) {
        output = CalibrationResultEnvelope{};
        output.request_id = command.request_id;
        output.status = CalibrationCommandStatus::Failed;
        return true;
      }
      output = CalibrationResultEnvelope{};
      output.protocol_version = command.protocol_version;
      output.request_id = command.request_id;
      output.session_id = command.session_id;
      output.status = CalibrationCommandStatus::Committed;
      output.profile = active;
      if (!replay_store_->save(command, output)) {
        output.status = CalibrationCommandStatus::Failed;
        return true;
      }
      output.status = CalibrationCommandStatus::Replayed;
      return true;
    }
  }
  return false;
}

bool CalibrationTransportController::remember(
    const CalibrationCommandEnvelope& command,
    const CalibrationResultEnvelope& result) noexcept {
  if (replay_store_ != nullptr &&
      command.operation == CalibrationOperation::Commit &&
      result.status == CalibrationCommandStatus::Committed &&
      !replay_store_->save(command, result)) {
    return false;
  }
  replay_[replay_next_].command = command;
  replay_[replay_next_].result = result;
  replay_[replay_next_].valid = true;
  replay_next_ = (replay_next_ + 1U) % replay_.size();
  return true;
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
         command.transport == owner_ && command.principal_id == principal_id_ && command.target == owner_target_;
}

void CalibrationTransportController::disconnect(
    CalibrationTransport transport, uint64_t now_us) noexcept {
  if (!owner_active_ || owner_ != transport) return;
  (void)workflow_.cancel();
  owner_active_ = false;
  session_id_ = 0U;
  principal_id_ = 0U;
  emit_owner(now_us, false, transport);
}

void CalibrationTransportController::expire(uint64_t now_us) noexcept {
  if (!owner_active_) return;
  CalibrationStatusSnapshot snapshot{};
  if (!workflow_.status(snapshot)) return;
  if (snapshot.last_activity_us == 0U || now_us < snapshot.last_activity_us ||
      now_us - snapshot.last_activity_us <= CalibrationSession::kSessionTimeoutUs) {
    return;
  }
  CalibrationCommandEnvelope expired{};
  expired.transport = owner_;
  expired.operation = CalibrationOperation::Cancel;
  (void)workflow_.cancel();
  owner_active_ = false;
  session_id_ = 0U;
  principal_id_ = 0U;
  emit_result(expired, CalibrationCommandStatus::Expired, now_us);
}

void CalibrationTransportController::emit_result(
    const CalibrationCommandEnvelope& command, CalibrationCommandStatus status,
    uint64_t now_us, const CalibrationProfileRef* profile) noexcept {
  if (debug_sink_ == nullptr) return;
  debug::EventInput event{};
  event.monotonic_us = now_us;
  event.module = debug::Module::Calibration;
  event.event_code = 50U;
  event.level = debug::Level::Info;
  const bool cancelled = command.operation == CalibrationOperation::Cancel &&
      (status == CalibrationCommandStatus::Accepted ||
       status == CalibrationCommandStatus::Committed ||
       status == CalibrationCommandStatus::Expired);
  event.arg0 = cancelled ? 2 :
      (status == CalibrationCommandStatus::Accepted ||
       status == CalibrationCommandStatus::Committed ||
       status == CalibrationCommandStatus::Replayed ? 0 : 1);
  // Failed combines workflow and replay-store failures; neither establishes an
  // internal-error cause, so leave the reason unknown in the wire event.
  event.arg1 = status == CalibrationCommandStatus::Expired ? 4 : 0;
  if (command.request_id != 0U && command.request_id <= UINT32_MAX) {
    event.request_id = static_cast<uint32_t>(command.request_id);
    event.presence = debug::kHasRequestId;
  }
  (void)debug_sink_->emit(event);
  if (command.operation == CalibrationOperation::Commit &&
      status == CalibrationCommandStatus::Committed && profile != nullptr &&
      profile->present) {
    event.event_code = 52U;
    event.arg0 = 0;
    event.arg1 = 10;
    event.request_id = 0U;
    event.presence = 0U;
    (void)debug_sink_->emit(event);
  }
  if (command.operation == CalibrationOperation::Start &&
      status == CalibrationCommandStatus::Accepted) {
    emit_owner(now_us, true, command.transport);
  } else if ((command.operation == CalibrationOperation::Commit &&
              status == CalibrationCommandStatus::Committed) ||
             (command.operation == CalibrationOperation::Cancel &&
              (status == CalibrationCommandStatus::Accepted ||
               status == CalibrationCommandStatus::Expired))) {
    emit_owner(now_us, false, command.transport);
  }
}

void CalibrationTransportController::emit_owner(
    uint64_t now_us, bool active, CalibrationTransport owner) noexcept {
  if (debug_sink_ == nullptr) return;
  debug::EventInput event{};
  event.monotonic_us = now_us;
  event.module = debug::Module::Calibration;
  event.event_code = 51U;
  event.level = debug::Level::Info;
  event.arg0 = !active ? 0 :
      owner == CalibrationTransport::Usb ? 1 :
      owner == CalibrationTransport::Ble ? 2 : 3;
  event.arg1 = active
      ? static_cast<int32_t>(CalibrationSession::kSessionTimeoutUs / 1000U)
      : 0;
  (void)debug_sink_->emit(event);
}

bool CalibrationTransportController::expected_profile_matches(
    const CalibrationCommandEnvelope& command) const noexcept {
  if (!command.expected_profile_present) return true;
  if (command.operation == CalibrationOperation::Commit) {
    CalibrationProfileRef proposed{};
    if (workflow_.proposed_profile(proposed) && proposed.present &&
        proposed.equals(command.expected_profile)) {
      return true;
    }
  }
  CalibrationProfileRef active{};
  if (!workflow_.active_profile(command.target, active)) return false;
  return active.equals(command.expected_profile);
}

bool CalibrationTransportController::execute(
    const CalibrationCommandEnvelope& command, uint64_t now_us,
    CalibrationResultEnvelope& output) noexcept {
  expire(now_us);
  fill(command, output);
  struct ResultObservation final {
    CalibrationTransportController& controller;
    const CalibrationCommandEnvelope& command;
    CalibrationResultEnvelope& result;
    uint64_t now_us;
    ~ResultObservation() noexcept {
      controller.emit_result(command, result.status, now_us, &result.profile);
    }
  } observation{*this, command, output, now_us};
  if (command.protocol_version != CalibrationCommandEnvelope::kProtocolVersion ||
      command.device_id != device_id_ || command.request_id == 0U) {
    output.status = CalibrationCommandStatus::Invalid;
    return false;
  }
  if (command.transport != CalibrationTransport::Usb &&
      (authorizer_ == nullptr || !authorizer_->authorize(command))) {
    output.status = CalibrationCommandStatus::Unauthorized;
    return false;
  }
  // Virtual observations cannot enter calibration workflow or replay state.
  if (build::kSensorSimulation && !read_only(command.operation)) {
    output.request_id = command.request_id;
    output.status = CalibrationCommandStatus::Failed;
    return false;
  }
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
  if (command.expires_at_us != 0U && now_us > command.expires_at_us) {
    output.status = CalibrationCommandStatus::Expired;
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
  if (command.target == CalibrationTarget::Mq135Gas && command.operation == CalibrationOperation::Rollback &&
      !command.expected_profile_present) { output.status = CalibrationCommandStatus::Invalid; return false; }
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
        owner_ = command.transport; owner_target_ = command.target;
        owner_active_ = true;
        session_id_ = command.session_id != 0U ? command.session_id
                                                : next_session_id_++;
        principal_id_ = command.principal_id;
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
      if (replay_store_ != nullptr && !replay_store_->prepare(command)) {
        output.status = CalibrationCommandStatus::Failed;
        return false;
      }
      ok = workflow_.commit();
      if (ok) {
        ok = workflow_.active_profile(command.target, output.profile) &&
             output.profile.present;
        owner_active_ = false;
        session_id_ = 0U;
        principal_id_ = 0U;
      }
      break;
    case CalibrationOperation::Cancel:
      ok = workflow_.cancel();
      if (ok) {
        owner_active_ = false;
        session_id_ = 0U;
        principal_id_ = 0U;
      }
      break;
    case CalibrationOperation::History:
      ok = workflow_.history(command.target, output);
      break;
    case CalibrationOperation::Rollback:
      ok = workflow_.rollback(command.target);
      if (ok) {
        ok = workflow_.active_profile(command.target, output.profile) && output.profile.present;
        owner_active_ = false; session_id_ = 0; principal_id_ = 0;
      }
      break;
  }
  if (ok && command.target == CalibrationTarget::Mq135Gas && read_only(command.operation)) {
    ok = workflow_.gas_diagnostic(command.target, now_us, output.gas);
    if (ok) {
      // MQG2 replaces MQG1 only when a normalized mode/profile exists.
      (void)workflow_.normalized_gas_diagnostic(command.target, now_us,
                                                output.normalized_gas);
    }
  }
  if (ok && (command.operation == CalibrationOperation::Commit || command.operation == CalibrationOperation::Rollback)) {
    output.status = CalibrationCommandStatus::Committed;
  } else {
    output.status = ok ? CalibrationCommandStatus::Accepted
                       : CalibrationCommandStatus::Failed;
  }
  if (command.request_id != 0U &&
      !remember(command, output)) {
    output.status = CalibrationCommandStatus::Failed;
    return false;
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
    principal_id_ = 0U;
  }
}

}  // namespace sigap::calibration
