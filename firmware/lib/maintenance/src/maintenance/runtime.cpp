#include "maintenance/runtime.h"

#include <charconv>
#include <cstdint>

#ifdef ARDUINO
#include <Arduino.h>
#endif

namespace sigap::maintenance {
namespace {

bool text_equals(const FixedText<64U>& value, const char* expected) noexcept {
  const std::string_view candidate = value.view();
  std::size_t length{0U};
  while (expected[length] != '\0') ++length;
  return candidate.size() == length && candidate ==
      std::string_view(expected, length);
}

const char* mode_name(DeviceMode mode) noexcept {
  switch (mode) {
    case DeviceMode::Run: return "RUN";
    case DeviceMode::Maintenance: return "MAINTENANCE";
    case DeviceMode::Calibrating: return "CALIBRATING";
    case DeviceMode::ConfigRequired: return "CONFIG_REQUIRED";
    case DeviceMode::RebootRequired: return "REBOOT_REQUIRED";
    case DeviceMode::StorageError: return "STORAGE_ERROR";
  }
  return "STORAGE_ERROR";
}

bool profile_target_from_text(const FixedText<64U>& text,
                              acquisition::ProfileTarget& target) noexcept {
  struct Entry { const char* name; acquisition::ProfileTarget target; };
  constexpr Entry entries[] = {
      {"hx711_organic", acquisition::ProfileTarget::Hx711Organic},
      {"hx711_inorganic", acquisition::ProfileTarget::Hx711Inorganic},
      {"hx711_b3", acquisition::ProfileTarget::Hx711B3},
      {"ultrasonic_organic", acquisition::ProfileTarget::UltrasonicOrganic},
      {"ultrasonic_inorganic", acquisition::ProfileTarget::UltrasonicInorganic},
      {"ultrasonic_b3", acquisition::ProfileTarget::UltrasonicB3},
      {"mq4", acquisition::ProfileTarget::Mq4},
      {"mq135", acquisition::ProfileTarget::Mq135},
      {"mq7", acquisition::ProfileTarget::Mq7},
      {"mq8", acquisition::ProfileTarget::Mq8},
      {"ds18b20_b3", acquisition::ProfileTarget::Ds18b20B3},
      {"sht31_44", acquisition::ProfileTarget::Sht31_44},
      {"sht31_45", acquisition::ProfileTarget::Sht31_45},
  };
  for (const Entry& entry : entries) {
    if (text_equals(text, entry.name)) {
      target = entry.target;
      return true;
    }
  }
  return false;
}

}  // namespace

#ifdef ARDUINO
bool ArduinoUsbTransport::available() noexcept { return Serial.available() > 0; }

int ArduinoUsbTransport::read() noexcept { return Serial.read(); }

bool ArduinoUsbTransport::write(std::string_view value) noexcept {
  return Serial.write(reinterpret_cast<const uint8_t*>(value.data()),
                      value.size()) == value.size();
}
#endif

bool CommandRouter::calibration_target_from_text(
    const FixedText<64U>& text,
    calibration::CalibrationTarget& target) noexcept {
  acquisition::ProfileTarget profile_target{};
  if (!profile_target_from_text(text, profile_target) ||
      profile_target == acquisition::ProfileTarget::Unknown) return false;
  target = static_cast<calibration::CalibrationTarget>(
      static_cast<uint8_t>(profile_target) - 1U);
  return true;
}

void CommandRouter::refresh_mode() noexcept {
  switch (config_.state()) {
    case config::ConfigState::Active: mode_ = DeviceMode::Run; break;
    case config::ConfigState::RebootRequired: mode_ = DeviceMode::RebootRequired; break;
    case config::ConfigState::ConfigRequired: mode_ = DeviceMode::ConfigRequired; break;
    case config::ConfigState::StorageError: mode_ = DeviceMode::StorageError; break;
    default:
      if (mode_ != DeviceMode::Maintenance && mode_ != DeviceMode::Calibrating) {
        mode_ = DeviceMode::Maintenance;
      }
      break;
  }
}

bool CommandRouter::field_from_text(const FixedText<64U>& text,
                                    config::ConfigField& field) noexcept {
  struct Entry { const char* name; config::ConfigField field; };
  constexpr Entry entries[] = {
      {"sampling.hx711_period_ms", config::ConfigField::Hx711PeriodMs},
      {"sampling.mq_period_ms", config::ConfigField::MqPeriodMs},
      {"sampling.ultrasonic_period_ms", config::ConfigField::UltrasonicPeriodMs},
      {"sampling.ds18b20_period_ms", config::ConfigField::Ds18b20PeriodMs},
      {"sampling.sht31_period_ms", config::ConfigField::Sht31PeriodMs},
      {"telemetry.summary_period_ms", config::ConfigField::SummaryPeriodMs},
      {"telemetry.retry_min_ms", config::ConfigField::RetryMinMs},
      {"telemetry.retry_max_ms", config::ConfigField::RetryMaxMs},
      {"telemetry.max_records_per_batch", config::ConfigField::MaxRecordsPerBatch},
      {"storage.flash_pending_quota_bytes", config::ConfigField::FlashPendingQuota},
      {"storage.sd_pending_quota_bytes", config::ConfigField::SdPendingQuota},
  };
  for (const Entry& entry : entries) {
    if (text_equals(text, entry.name)) {
      field = entry.field;
      return true;
    }
  }
  return false;
}

bool CommandRouter::append_u64(MaintenanceResponse& response,
                               const char* label, uint64_t value) noexcept {
  char buffer[21]{};
  const auto result = std::to_chars(buffer, buffer + sizeof(buffer), value);
  if (result.ec != std::errc{}) return false;
  return response.append(label) &&
         response.append(std::string_view(buffer,
                                          static_cast<std::size_t>(result.ptr - buffer))) &&
         response.append("\n");
}

bool CommandRouter::append_hash(MaintenanceResponse& response,
                                const config::ConfigHash& hash) noexcept {
  static constexpr char digits[] = "0123456789abcdef";
  char output[64]{};
  for (std::size_t i = 0U; i < hash.size(); ++i) {
    output[i * 2U] = digits[hash[i] >> 4U];
    output[i * 2U + 1U] = digits[hash[i] & 0x0fU];
  }
  return response.append("config_hash=") && response.append(
      std::string_view(output, sizeof(output))) && response.append("\n");
}

CommandStatus CommandRouter::execute_config(const MaintenanceCommand& command,
                                            MaintenanceResponse& response) noexcept {
  switch (command.kind) {
    case CommandKind::ConfigActive:
      if (!append_u64(response, "config_version=", config_.identity().config_version) ||
          !append_hash(response, config_.identity().hash)) return CommandStatus::Failed;
      return CommandStatus::Accepted;
    case CommandKind::ConfigDraftStart:
      if (config_.state() == config::ConfigState::ConfigRequired) {
        return config_.start_factory_draft() ? CommandStatus::Accepted : CommandStatus::Rejected;
      }
      return config_.start_draft() ? CommandStatus::Accepted : CommandStatus::Rejected;
    case CommandKind::ConfigSet: {
      config::ConfigField field{};
      if (!command.has_numeric || !field_from_text(command.field, field) ||
          command.numeric_value < 0) return CommandStatus::InvalidSyntax;
      return config_.set_draft_field(field, static_cast<uint64_t>(command.numeric_value))
                 ? CommandStatus::Accepted : CommandStatus::Rejected;
    }
    case CommandKind::ConfigValidate:
      return config_.validate_draft_and_enter() ? CommandStatus::Accepted : CommandStatus::Rejected;
    case CommandKind::ConfigPreview:
      return config_.preview_draft() ? CommandStatus::Accepted : CommandStatus::Rejected;
    case CommandKind::ConfigCommit:
      if (!config_.commit_draft()) return CommandStatus::Rejected;
      mode_ = DeviceMode::RebootRequired;
      return CommandStatus::Accepted;
    case CommandKind::ConfigCancel:
      return config_.cancel_draft() ? CommandStatus::Accepted : CommandStatus::Rejected;
    case CommandKind::ConfigReset:
      if (!config_.has_draft() && config_.state() == config::ConfigState::ConfigRequired &&
          !config_.start_factory_draft()) return CommandStatus::Rejected;
      return config_.reset_draft() ? CommandStatus::Accepted : CommandStatus::Rejected;
    default: return CommandStatus::Rejected;
  }
}

CommandStatus CommandRouter::execute(const MaintenanceCommand& command,
                                     uint64_t,
                                     MaintenanceResponse& response) noexcept {
  refresh_mode();
  switch (command.kind) {
    case CommandKind::Status:
      if (!response.append("mode=") || !response.append(mode_name(mode_)) ||
          !response.append("\n") ||
          !append_u64(response, "config_version=", config_.identity().config_version) ||
          !append_hash(response, config_.identity().hash)) {
        return CommandStatus::Failed;
      }
      return CommandStatus::Accepted;
    case CommandKind::MaintenanceEnter:
      mode_ = DeviceMode::Maintenance;
      return CommandStatus::Accepted;
    case CommandKind::MaintenanceExit:
      refresh_mode();
      if (config_.state() == config::ConfigState::Active) {
        mode_ = DeviceMode::Run;
        return CommandStatus::Accepted;
      }
      return CommandStatus::Rejected;
    case CommandKind::SecretStatus: {
      const auto presence = secrets_.status();
      if (!response.append_presence("wifi", presence.wifi_configured) ||
          !response.append_presence("device_token", presence.device_token_configured)) {
        return CommandStatus::Failed;
      }
      return CommandStatus::Accepted;
    }
    case CommandKind::SecretSetWifi:
      return secrets_.set_wifi(command.target.c_str(), command.value.c_str())
                 ? CommandStatus::Accepted : CommandStatus::Rejected;
    case CommandKind::SecretRotateToken:
      return secrets_.rotate_device_token(command.value.c_str())
                 ? CommandStatus::Accepted : CommandStatus::Rejected;
    case CommandKind::SecretClear:
      return secrets_.clear_all(command.value.view() == "CONFIRM")
                 ? CommandStatus::Accepted : CommandStatus::Rejected;
    case CommandKind::CalibrationStart: {
      calibration::CalibrationTarget target{};
      if (!calibration_target_from_text(command.target, target)) return CommandStatus::InvalidSyntax;
      calibration::CaptureCriteria criteria{calibration::Hx711CaptureCriteria{1U, INT64_MAX}};
      switch (target) {
        case calibration::CalibrationTarget::UltrasonicOrganic:
        case calibration::CalibrationTarget::UltrasonicInorganic:
        case calibration::CalibrationTarget::UltrasonicB3:
          criteria = calibration::DistanceCaptureCriteria{1U, 3.402823e+38F};
          break;
        case calibration::CalibrationTarget::Mq4:
        case calibration::CalibrationTarget::Mq135:
        case calibration::CalibrationTarget::Mq7:
        case calibration::CalibrationTarget::Mq8:
          criteria = calibration::MqCaptureCriteria{1U, UINT16_MAX,
              target == calibration::CalibrationTarget::Mq7};
          break;
        default: break;
      }
      if (!calibration_.start(target, criteria, 0U)) return CommandStatus::Rejected;
      mode_ = DeviceMode::Calibrating;
      return CommandStatus::Accepted;
    }
    case CommandKind::CalibrationStatus:
      if (!response.append("calibration_active=") ||
          !response.append(calibration_.state() == calibration::CalibrationState::Idle
                               ? "false\n" : "true\n")) return CommandStatus::Failed;
      return CommandStatus::Accepted;
    case CommandKind::CalibrationCapture:
      if (!command.has_numeric) return CommandStatus::InvalidSyntax;
      if (calibration_.target() <= calibration::CalibrationTarget::Hx711B3) {
        return calibration_.observe(calibration::Hx711Trial{
                    command.numeric_value, acquisition::SampleQuality::Ready})
                   ? CommandStatus::Accepted : CommandStatus::Rejected;
      }
      if (calibration_.target() >= calibration::CalibrationTarget::Mq4 &&
          calibration_.target() <= calibration::CalibrationTarget::Mq8) {
        if (command.numeric_value < 0 || command.numeric_value > UINT16_MAX)
          return CommandStatus::InvalidNumeric;
        return calibration_.observe(calibration::MqTrial{
                    static_cast<uint16_t>(command.numeric_value),
                    acquisition::SampleQuality::Ready, true})
                   ? CommandStatus::Accepted : CommandStatus::Rejected;
      }
      return CommandStatus::Rejected;
    case CommandKind::CalibrationValidate:
      return calibration_.validate() ? CommandStatus::Accepted : CommandStatus::Rejected;
    case CommandKind::CalibrationPreview:
      return calibration_.preview() ? CommandStatus::Accepted : CommandStatus::Rejected;
    case CommandKind::CalibrationCommit:
      return calibration_.request_commit() ? CommandStatus::Accepted : CommandStatus::Rejected;
    case CommandKind::CalibrationCancel:
      if (!calibration_.cancel()) return CommandStatus::Rejected;
      refresh_mode();
      return CommandStatus::Accepted;
    case CommandKind::ProfileStatus:
    case CommandKind::ProfileExport:
    case CommandKind::ProfileImport:
    case CommandKind::ProfileRollback:
    case CommandKind::ProfileReset: {
      if (profiles_ == nullptr) return CommandStatus::Rejected;
      acquisition::ProfileTarget target{};
      if (!profile_target_from_text(command.target, target)) return CommandStatus::InvalidSyntax;
      if (command.kind == CommandKind::ProfileStatus) {
        const auto state = profiles_->state(target);
        if (!response.append("profile_state=") ||
            !response.append(state == acquisition::ProfileManagerState::Active ? "ACTIVE" :
                             state == acquisition::ProfileManagerState::Draft ? "DRAFT" :
                             state == acquisition::ProfileManagerState::Preview ? "PREVIEW" :
                             state == acquisition::ProfileManagerState::Error ? "ERROR" : "MISSING") ||
            !response.append("\n")) return CommandStatus::Failed;
        acquisition::ActiveProfileSnapshot snapshot{};
        if (!profiles_->snapshot(target, snapshot)) return CommandStatus::Failed;
        if (snapshot.present) {
          if (!append_u64(response, "profile_version=", snapshot.ref.profile_version) ||
              !response.append("profile_hash_present=true\n")) return CommandStatus::Failed;
        }
        return CommandStatus::Accepted;
      }
      if (command.kind == CommandKind::ProfileRollback) {
        return profiles_->rollback(target) ? CommandStatus::Accepted : CommandStatus::Rejected;
      }
      if (command.kind == CommandKind::ProfileReset) {
        return profiles_->reset(target) ? CommandStatus::Accepted : CommandStatus::Rejected;
      }
      // Import/export payload transfer is deliberately kept behind the typed
      // ProfileManager API; this line protocol does not accept unbounded blobs.
      return CommandStatus::Rejected;
    }
    case CommandKind::Reboot:
      mode_ = DeviceMode::RebootRequired;
      return CommandStatus::Accepted;
    case CommandKind::ConfigActive:
    case CommandKind::ConfigDraftStart:
    case CommandKind::ConfigSet:
    case CommandKind::ConfigValidate:
    case CommandKind::ConfigPreview:
    case CommandKind::ConfigCommit:
    case CommandKind::ConfigCancel:
    case CommandKind::ConfigReset:
      return execute_config(command, response);
    default:
      // Calibration/profile commands are intentionally routed to their future
      // managers; rejecting them here is safer than mutating a profile without
      // validation or activation metadata.
      return CommandStatus::Rejected;
  }
}

}  // namespace sigap::maintenance
