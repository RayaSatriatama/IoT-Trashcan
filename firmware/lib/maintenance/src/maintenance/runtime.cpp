#include "maintenance/runtime.h"
#include "sigap/build/build_config.h"
#include <algorithm>
#include <charconv>
#include <cstdint>
#include <cmath>
#include <limits>

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

bool is_calibration_mutation(CommandKind kind) noexcept {
  switch (kind) {
    case CommandKind::CalibrationStart:
    case CommandKind::CalibrationCriteria:
    case CommandKind::CalibrationCapture:
    case CommandKind::CalibrationValidate:
    case CommandKind::CalibrationPreview:
    case CommandKind::CalibrationCommit:
    case CommandKind::CalibrationCancel:
      return true;
    default:
      return false;
  }
}

bool is_sensor_profile_mutation(CommandKind kind) noexcept {
  switch (kind) {
    case CommandKind::ProfileImport:
    case CommandKind::ProfileImportBegin:
    case CommandKind::ProfileImportChunk:
    case CommandKind::ProfileImportFinish:
    case CommandKind::ProfileValidate:
    case CommandKind::ProfilePreview:
    case CommandKind::ProfileCommit:
    case CommandKind::ProfileRollback:
    case CommandKind::ProfileReset:
      return true;
    default:
      return false;
  }
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

acquisition::ConfigRef config_ref_from_identity(
    const config::ConfigIdentity& identity) noexcept {
  acquisition::ConfigRef ref{};
  ref.schema_version = identity.schema_version;
  ref.config_version = identity.config_version;
  ref.hash = identity.hash;
  ref.present = identity.has_slot;
  return ref;
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
      {"capacity_organic", acquisition::ProfileTarget::Hx711Organic},
      {"capacity_inorganic", acquisition::ProfileTarget::Hx711Inorganic},
      {"capacity_b3", acquisition::ProfileTarget::Hx711B3},
  };
  for (const Entry& entry : entries) {
    if (text_equals(text, entry.name)) {
      target = entry.target;
      return true;
    }
  }
  return false;
}

bool profile_key_from_text(const FixedText<64U>& text,
                           acquisition::ProfileKey& key) noexcept {
  const std::string_view value = text.view();
  if (value == "mq135_gas") { key = acquisition::kMq135GasKey; return true; }
  const auto separator = value.find(':');
  FixedText<64U> base{};
  if (!base.assign(value.substr(0U, separator == std::string_view::npos
                                      ? value.size() : separator))) return false;
  if (!profile_target_from_text(base, key.target)) return false;
  key.kind = acquisition::default_profile_kind(key.target);
  if (text_equals(base, "capacity_organic") ||
      text_equals(base, "capacity_inorganic") ||
      text_equals(base, "capacity_b3")) {
    key.kind = acquisition::ProfileKind::Capacity;
  }
  if (separator != std::string_view::npos) {
    if (value.substr(separator + 1U) == "heater") {
      key.kind = acquisition::ProfileKind::Heater;
    } else if (value.substr(separator + 1U) == "capacity") {
      key.kind = acquisition::ProfileKind::Capacity;
    } else if (value.substr(separator + 1U) != "adc") {
      return false;
    }
  }
  return true;
}

int hex_digit(char value) noexcept {
  if (value >= '0' && value <= '9') return value - '0';
  if (value >= 'a' && value <= 'f') return value - 'a' + 10;
  if (value >= 'A' && value <= 'F') return value - 'A' + 10;
  return -1;
}

bool decode_hex(std::string_view text, uint8_t* output,
                std::size_t capacity, std::size_t& written) noexcept {
  if (output == nullptr || (text.size() % 2U) != 0U || text.size() / 2U > capacity) return false;
  written = text.size() / 2U;
  for (std::size_t i = 0U; i < written; ++i) {
    const int high = hex_digit(text[i * 2U]);
    const int low = hex_digit(text[i * 2U + 1U]);
    if (high < 0 || low < 0) return false;
    output[i] = static_cast<uint8_t>((high << 4) | low);
  }
  return true;
}

bool decode_hash(std::string_view text, config::ConfigHash& output) noexcept {
  if (text.substr(0U, 5U) == "hash=") text.remove_prefix(5U);
  std::size_t written{0U};
  return text.size() == output.size() * 2U &&
         decode_hex(text, output.data(), output.size(), written) &&
         written == output.size();
}

bool is_ultrasonic_target(calibration::CalibrationTarget target) noexcept {
  return target >= calibration::CalibrationTarget::UltrasonicOrganic &&
         target <= calibration::CalibrationTarget::UltrasonicB3;
}

bool is_mq_target(calibration::CalibrationTarget target) noexcept {
  return target >= calibration::CalibrationTarget::Mq4 &&
         target <= calibration::CalibrationTarget::Mq8;
}

bool is_sht_target(calibration::CalibrationTarget target) noexcept {
  return target == calibration::CalibrationTarget::Sht31_44 ||
         target == calibration::CalibrationTarget::Sht31_45;
}

bool append_hex(MaintenanceResponse& response, const uint8_t* bytes,
                std::size_t size) noexcept {
  static constexpr char digits[] = "0123456789abcdef";
  for (std::size_t i = 0U; i < size; ++i) {
    char output[2]{digits[bytes[i] >> 4U], digits[bytes[i] & 0x0fU]};
    if (!response.append(std::string_view(output, 2U))) return false;
  }
  return true;
}

}  // namespace

#ifdef ARDUINO
bool ArduinoUsbTransport::available() noexcept { return Serial.available() > 0; }

int ArduinoUsbTransport::read() noexcept { return Serial.read(); }

bool ArduinoUsbTransport::write(std::string_view value) noexcept {
  return Serial.write(reinterpret_cast<const uint8_t*>(value.data()),
                      value.size()) == value.size();
}

bool ArduinoUsbTransport::connected() noexcept {
  // With ARDUINO_USB_MODE=1 this reports the native ESP32-S3 USB
  // Serial/JTAG connection. An external USB-UART bridge does not provide
  // this signal and is intentionally not treated as native maintenance USB.
  return static_cast<bool>(Serial);
}

void ArduinoRebootController::request_reboot() noexcept {
  // Defer the actual reset until the application has flushed the response
  // produced by MaintenanceService.  Calling ESP.restart() here can truncate
  // the acknowledgement on the USB transport.
  requested_ = true;
}
#endif

bool CommandRouter::calibration_target_from_text(
    const FixedText<64U>& text,
    calibration::CalibrationTarget& target) noexcept {
  if (text.view() == "mq135_gas") { target = calibration::CalibrationTarget::Mq135Gas; return true; }
  acquisition::ProfileTarget profile_target{};
  if (!profile_target_from_text(text, profile_target) ||
      profile_target == acquisition::ProfileTarget::Unknown) return false;
  target = static_cast<calibration::CalibrationTarget>(
      static_cast<uint8_t>(profile_target) - 1U);
  return true;
}

void CommandRouter::refresh_mode() noexcept {
  const auto calibration_state = calibration_.state();
  if (calibration_state != calibration::CalibrationState::Idle &&
      calibration_state != calibration::CalibrationState::Failed) {
    mode_ = DeviceMode::Calibrating;
    return;
  }
  if (maintenance_authorized_) {
    mode_ = DeviceMode::Maintenance;
    return;
  }
  switch (config_.state()) {
    case config::ConfigState::Active:
      mode_ = DeviceMode::Run;
      break;
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
                               const char* label, uint64_t value,
                               bool newline) noexcept {
  char buffer[21]{};
  const auto result = std::to_chars(buffer, buffer + sizeof(buffer), value);
  if (result.ec != std::errc{}) return false;
  if (!response.append(label) ||
      !response.append(std::string_view(
          buffer, static_cast<std::size_t>(result.ptr - buffer)))) {
    return false;
  }
  return !newline || response.append("\n");
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
      if (audit_ != nullptr) {
        audit_->config_committed(
            config_ref_from_identity(config_.pending_identity()));
      }
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

bool CommandRouter::is_calibrating(acquisition::SensorId sensor) const noexcept {
  const auto state = calibration_.state();
  if (state == calibration::CalibrationState::Idle ||
      state == calibration::CalibrationState::Failed) return false;
  acquisition::SensorId expected = acquisition::SensorId::Hx711Organic;
  switch (calibration_.target()) {
    case calibration::CalibrationTarget::Mq135Gas: return false; // Reference sampling does not recalibrate ADC or another sensor.
    case calibration::CalibrationTarget::Hx711Organic: expected = acquisition::SensorId::Hx711Organic; break;
    case calibration::CalibrationTarget::Hx711Inorganic: expected = acquisition::SensorId::Hx711Inorganic; break;
    case calibration::CalibrationTarget::Hx711B3: expected = acquisition::SensorId::Hx711B3; break;
    case calibration::CalibrationTarget::UltrasonicOrganic: expected = acquisition::SensorId::UltrasonicOrganic; break;
    case calibration::CalibrationTarget::UltrasonicInorganic: expected = acquisition::SensorId::UltrasonicInorganic; break;
    case calibration::CalibrationTarget::UltrasonicB3: expected = acquisition::SensorId::UltrasonicB3; break;
    case calibration::CalibrationTarget::Mq4: expected = acquisition::SensorId::Mq4; break;
    case calibration::CalibrationTarget::Mq135: expected = acquisition::SensorId::Mq135; break;
    case calibration::CalibrationTarget::Mq7: expected = acquisition::SensorId::Mq7; break;
    case calibration::CalibrationTarget::Mq8: expected = acquisition::SensorId::Mq8; break;
    case calibration::CalibrationTarget::Ds18b20B3: expected = acquisition::SensorId::Ds18b20B3; break;
    case calibration::CalibrationTarget::Sht31_44: expected = acquisition::SensorId::Sht31_44; break;
    case calibration::CalibrationTarget::Sht31_45: expected = acquisition::SensorId::Sht31_45; break;
  }
  return sensor == expected;
}

void CommandRouter::cancel_profile_transfer() noexcept {
  if (profiles_ != nullptr && transfer_draft_active_) {
    (void)profiles_->cancel(transfer_key_);
  }
  transfer_import_active_ = false;
  transfer_export_active_ = false;
  transfer_draft_active_ = false;
  transfer_expected_hash_present_ = false;
  transfer_expected_size_ = 0U;
  transfer_received_size_ = 0U;
  transfer_export_offset_ = 0U;
  transfer_last_activity_us_ = 0U;
  transfer_profile_ = acquisition::EncodedProfile{};
}

void CommandRouter::poll(uint64_t now_us) noexcept {
  if (calibration_.tick(now_us)) {
    clear_calibration_proposal();
    criteria_set_ = false;
    evidence_sequence_ = 0U;
    mode_ = DeviceMode::Maintenance;
  }
  if ((transfer_import_active_ || transfer_export_active_ || transfer_draft_active_) &&
      now_us >= transfer_last_activity_us_ &&
      now_us - transfer_last_activity_us_ > kTransferTimeoutUs) {
    cancel_profile_transfer();
  }
  if (models_ != nullptr) models_->poll_model(now_us);
}

void CommandRouter::transport_lost() noexcept {
  maintenance_authorized_ = false;
  calibration_.abort();
  clear_calibration_proposal();
  criteria_set_ = false;
  evidence_sequence_ = 0U;
  cancel_profile_transfer();
  if (models_ != nullptr) models_->cancel_model();
  refresh_mode();
}

CommandStatus CommandRouter::profile_transfer(
    const MaintenanceCommand& command, MaintenanceResponse& response) noexcept {
  if (profiles_ == nullptr) return CommandStatus::Rejected;
  if (command.kind == CommandKind::ProfileExport) {
    acquisition::ProfileKey key{};
    if (!profile_key_from_text(command.target, key)) return CommandStatus::InvalidSyntax;
    if (!transfer_export_active_ || !(key == transfer_key_)) {
      if (!profiles_->export_active(key, transfer_profile_)) return CommandStatus::Rejected;
      transfer_key_ = key;
      transfer_export_offset_ = 0U;
      transfer_export_active_ = true;
    }
    if (transfer_export_offset_ == 0U) {
      if (!response.append("PROFILE BEGIN ") ||
          !append_u64(response, "size=", transfer_profile_.size, false) ||
          !response.append(" hash=") ||
          !append_hex(response, transfer_profile_.hash().data(), transfer_profile_.hash().size()) ||
          !response.append("\nDATA ")) return CommandStatus::Failed;
    } else if (!response.append("DATA ")) {
      return CommandStatus::Failed;
    }
    const std::size_t remaining = transfer_profile_.size - transfer_export_offset_;
    if (response.size() + 1U >= kMaxResponseBytes) return CommandStatus::Failed;
    const std::size_t capacity = (kMaxResponseBytes - response.size() - 1U) / 2U;
    const std::size_t chunk = std::min(remaining, capacity);
    if (chunk == 0U) return CommandStatus::Failed;
    if (!append_hex(response, transfer_profile_.bytes.data() + transfer_export_offset_, chunk) ||
        !response.append("\n")) return CommandStatus::Failed;
    transfer_export_offset_ += chunk;
    if (transfer_export_offset_ >= transfer_profile_.size) transfer_export_active_ = false;
    return CommandStatus::Accepted;
  }
  if (command.kind == CommandKind::ProfileImportBegin) {
    acquisition::ProfileKey key{};
    if (!profile_key_from_text(command.target, key) || !command.has_numeric ||
        command.numeric_value <= 0 ||
        static_cast<std::size_t>(command.numeric_value) > transfer_profile_.bytes.size()) {
      return CommandStatus::InvalidSyntax;
    }
    config::ConfigHash expected_hash{};
    if (!decode_hash(command.value.view(), expected_hash)) {
      return CommandStatus::InvalidSyntax;
    }
    transfer_key_ = key;
    transfer_expected_hash_ = expected_hash;
    transfer_expected_hash_present_ = true;
    transfer_expected_size_ = static_cast<std::size_t>(command.numeric_value);
    transfer_received_size_ = 0U;
    transfer_profile_ = acquisition::EncodedProfile{};
    transfer_import_active_ = true;
    return CommandStatus::Accepted;
  }
  if (command.kind == CommandKind::ProfileImportChunk) {
    if (!transfer_import_active_) return CommandStatus::Rejected;
    if (command.has_numeric &&
        command.numeric_value != static_cast<int64_t>(transfer_received_size_)) {
      cancel_profile_transfer();
      return CommandStatus::InvalidSyntax;
    }
    std::size_t written{0U};
    std::array<uint8_t, acquisition::kEncodedProfileBytes> chunk_bytes{};
    if (command.value.view().empty() ||
        !decode_hex(command.value.view(), chunk_bytes.data(), chunk_bytes.size(), written) ||
        transfer_received_size_ + written > transfer_expected_size_) {
      cancel_profile_transfer();
      return CommandStatus::InvalidSyntax;
    }
    std::copy_n(chunk_bytes.data(), written,
                transfer_profile_.bytes.begin() + transfer_received_size_);
    transfer_received_size_ += written;
    return CommandStatus::Accepted;
  }
  if (command.kind == CommandKind::ProfileImportFinish) {
    acquisition::ProfileKey finish_key{};
    if (!profile_key_from_text(command.target, finish_key) ||
        !(finish_key == transfer_key_)) {
      cancel_profile_transfer();
      return CommandStatus::InvalidSyntax;
    }
    if (!transfer_import_active_ || transfer_received_size_ != transfer_expected_size_) {
      cancel_profile_transfer();
      return CommandStatus::Rejected;
    }
    transfer_profile_.size = transfer_received_size_;
    transfer_import_active_ = false;
    if (!transfer_expected_hash_present_ ||
        transfer_profile_.hash() != transfer_expected_hash_) {
      cancel_profile_transfer();
      return CommandStatus::Rejected;
    }
    transfer_expected_hash_present_ = false;
    // Finish only stages a draft. Validation, preview, and activation are
    // explicit commands so an import cannot change runtime state implicitly.
    if (!profiles_->stage_import(transfer_key_, transfer_profile_)) {
      cancel_profile_transfer();
      return CommandStatus::Rejected;
    }
    transfer_draft_active_ = true;
    return CommandStatus::Accepted;
  }
  return CommandStatus::Rejected;
}

CommandStatus CommandRouter::execute(const MaintenanceCommand& command,
                                     uint64_t now_us,
                                     MaintenanceResponse& response) noexcept {
  // All transports share this boundary; virtual observations cannot become
  // persisted physical calibration or replace the stored sensor profiles.
  if (build::kSensorSimulation &&
      (is_calibration_mutation(command.kind) || is_sensor_profile_mutation(command.kind))) {
    (void)response.append("ERR SENSOR_SIMULATION\n");
    return CommandStatus::Rejected;
  }
  switch (command.kind) {
    case CommandKind::DebugMeta:
    case CommandKind::DebugPage:
    case CommandKind::DebugLevel:
      if (debug_commands_ == nullptr) {
        (void)response.append("ERR DEBUG_UNAVAILABLE\n");
        return CommandStatus::Rejected;
      }
      return debug_commands_->execute_debug(command, now_us, response);
    default: break;
  }
  if (io_gate_ != nullptr && io_gate_->active(debug::IoOwner::DebugHttp) &&
      command.kind != CommandKind::Status &&
      command.kind != CommandKind::SensorStatus &&
      command.kind != CommandKind::ModelStatus &&
      command.kind != CommandKind::ModelCancel &&
      command.kind != CommandKind::CalibrationCancel) {
    (void)response.append("ERR STORAGE_BUSY\n");
    return CommandStatus::Rejected;
  }
  const acquisition::ConfigRef current_config =
      config_ref_from_identity(config_.identity());
  if (audit_ != nullptr) audit_->set_context(now_us, current_config);
  if (profiles_ != nullptr) {
    profiles_->set_audit_context(now_us, current_config);
  }
  refresh_mode();
  if (is_calibration_mutation(command.kind) &&
      calibration_controller_ != nullptr &&
      !internal_calibration_dispatch_ && calibration_controller_->active()) {
    (void)response.append("ERR CALIBRATION_SESSION_BUSY\n");
    return CommandStatus::Rejected;
  }
  if (command.kind == CommandKind::ProfileImportBegin) {
    cancel_profile_transfer();
  }
  if (command.kind == CommandKind::ProfileExport ||
      command.kind == CommandKind::ProfileImportBegin ||
      command.kind == CommandKind::ProfileImportChunk ||
      command.kind == CommandKind::ProfileImportFinish) {
    transfer_last_activity_us_ = now_us;
  }
  (void)calibration_.tick(now_us);
  switch (command.kind) {
    case CommandKind::Status:
      if (!response.append("mode=") || !response.append(mode_name(mode_)) ||
          !response.append("\n") ||
          !append_u64(response, "config_version=", config_.identity().config_version) ||
          !append_hash(response, config_.identity().hash)) {
        return CommandStatus::Failed;
      }
      return CommandStatus::Accepted;
    case CommandKind::SensorStatus:
      return sensor_status(command, now_us, response);
    case CommandKind::MaintenanceEnter:
      maintenance_authorized_ = true;
      mode_ = DeviceMode::Maintenance;
      return CommandStatus::Accepted;
    case CommandKind::MaintenanceExit:
      maintenance_authorized_ = false;
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
      if (!calibration_.begin(target, now_us)) return CommandStatus::Rejected;
      clear_calibration_proposal();
      pending_target_ = target;
      gas_parameters_ = calibration::Mq135ReferenceParameters{};
      normalized_parameters_ = calibration::Mq135NormalizedParameters{};
      normalized_mode_ = false;
      evidence_sequence_ = 0;
      criteria_set_ = false;
      criteria_samples_ = 0U;
      criteria_span_ = 0;
      criteria_span_set_ = false;
      criteria_temperature_span_ = 0;
      criteria_humidity_span_ = 0;
      criteria_temperature_span_set_ = false;
      criteria_humidity_span_set_ = false;
      reference_mass_mg_ = 0U;
      reference_temperature_centi_c_ = 0;
      reference_humidity_centi_pct_ = 0;
      reference_temperature_set_ = false;
      reference_humidity_set_ = false;
      ultrasonic_echo_mm_per_us_milli_ = 0U;
      ultrasonic_echo_timeout_us_ = 0U;
      ultrasonic_inter_sensor_gap_us_ = 0U;
      ultrasonic_overshoot_tolerance_mm_milli_ = 0U;
      ultrasonic_echo_conversion_set_ = false;
      ultrasonic_tolerance_set_ = false;
      ultrasonic_timing_set_ = false;
      mq_reference_mv_ = 0U;
      mq_full_scale_counts_ = 0U;
      mq_divider_numerator_ = 0U;
      mq_divider_denominator_ = 0U;
      mq_engineering_set_ = false;
      ds_resolution_bits_ = 12U;
      ds_resolution_set_ = false;
      ds_rom_ = {};
      ds_rom_set_ = false;
      mode_ = DeviceMode::Calibrating;
      return CommandStatus::Accepted;
    }
    case CommandKind::CalibrationCriteria: {
      // Any accepted or attempted session-input update invalidates a prior
      // proposal. A new validation and preview is required before persistence.
      clear_calibration_proposal();
      const auto field = command.field.view();
      if (pending_target_ == calibration::CalibrationTarget::Mq135Gas) {
        // Declared environment may follow the completed required set as long as
        // no evidence has been admitted yet; it never mutates admitted data.
        if (criteria_set_ && normalized_mode_ &&
            (field == "reference_temperature_centi_c" ||
             field == "reference_humidity_centi_pct")) {
          if (!command.has_numeric) return CommandStatus::InvalidNumeric;
          const bool temperature = field == "reference_temperature_centi_c";
          if (temperature && (command.numeric_value < std::numeric_limits<int32_t>::min() ||
                              command.numeric_value > std::numeric_limits<int32_t>::max())) {
            return CommandStatus::InvalidNumeric;
          }
          if (!temperature && (command.numeric_value < 0 ||
                               command.numeric_value > 10000)) {
            return CommandStatus::InvalidNumeric;
          }
          return calibration_.update_normalized_environment(
                     temperature, temperature ? static_cast<int32_t>(command.numeric_value) : 0,
                     !temperature, !temperature ? static_cast<uint32_t>(command.numeric_value) : 0U)
                     ? CommandStatus::Accepted
                     : CommandStatus::Rejected;
        }
        if (criteria_set_ || calibration_.state() != calibration::CalibrationState::Configuring) return CommandStatus::Rejected;
        char text[32]{};
        std::string_view value = command.value.view();
        if (command.has_numeric) {
          const auto parsed = std::to_chars(text, text + sizeof(text), command.numeric_value);
          if (parsed.ec != std::errc{}) return CommandStatus::InvalidNumeric;
          value = std::string_view(text, parsed.ptr - text);
        }
        // `response_mode=normalized` switches this same target/session to the
        // typed normalized criteria; its absence keeps exact legacy behaviour.
        if (field == "response_mode" && !normalized_mode_) {
          gas_parameters_ = calibration::Mq135ReferenceParameters{};
          normalized_mode_ = true;
        }
        if (normalized_mode_) {
          // Optional environment keys are accepted after the required set.
          if (normalized_parameters_.complete() &&
              (field == "reference_temperature_centi_c" ||
               field == "reference_humidity_centi_pct")) {
            auto criteria = normalized_parameters_.criteria();
            if (!command.has_numeric ||
                !calibration::mq135_normalized_optional_environment(
                    criteria, field, command.numeric_value)) {
              return CommandStatus::InvalidNumeric;
            }
            return configure_normalized_reference(criteria, now_us)
                       ? CommandStatus::Accepted
                       : CommandStatus::Rejected;
          }
          const auto status = normalized_parameters_.set(field, value);
          if (status == calibration::Mq135NormalizedParameters::Status::Complete) {
            return configure_normalized_reference(normalized_parameters_.criteria(),
                                                  now_us)
                       ? CommandStatus::Accepted
                       : CommandStatus::Rejected;
          }
          if (status == calibration::Mq135NormalizedParameters::Status::Incomplete) {
            return CommandStatus::Accepted;
          }
          // A rejected normalized key must not leave a half-configured session.
          normalized_mode_ = false;
          normalized_parameters_ = calibration::Mq135NormalizedParameters{};
          return CommandStatus::InvalidSyntax;
        }
        if (!gas_parameters_.set(field, value)) return CommandStatus::InvalidSyntax;
        if (!gas_parameters_.complete()) return CommandStatus::Accepted;
        return configure_gas_reference(gas_parameters_.criteria(), now_us) ? CommandStatus::Accepted : CommandStatus::Rejected;
      }
      if (field == "reference_temperature_centi_c") {
        if (command.numeric_value < std::numeric_limits<int32_t>::min() ||
            command.numeric_value > std::numeric_limits<int32_t>::max()) {
          return CommandStatus::InvalidNumeric;
        }
        reference_temperature_centi_c_ = static_cast<int32_t>(command.numeric_value);
        reference_temperature_set_ = true;
      } else if (field == "reference_humidity_centi_pct") {
        if (command.numeric_value < 0 ||
            command.numeric_value > std::numeric_limits<int32_t>::max()) {
          return CommandStatus::InvalidNumeric;
        }
        reference_humidity_centi_pct_ = static_cast<int32_t>(command.numeric_value);
        reference_humidity_set_ = true;
      } else if (field == "resolution_bits") {
        if (command.numeric_value < 9 || command.numeric_value > 12) {
          return CommandStatus::InvalidNumeric;
        }
        ds_resolution_bits_ = static_cast<uint8_t>(command.numeric_value);
        ds_resolution_set_ = true;
      } else if (field == "echo_mm_per_us_milli") {
        if (command.numeric_value <= 0 || command.numeric_value > 1000) {
          return CommandStatus::InvalidNumeric;
        }
        ultrasonic_echo_mm_per_us_milli_ =
            static_cast<uint32_t>(command.numeric_value);
        ultrasonic_echo_conversion_set_ = true;
        if (evidence_ != nullptr &&
            !evidence_->set_ultrasonic_engineering_conversion(
                pending_target_, static_cast<float>(command.numeric_value) /
                                1000.0F)) {
          return CommandStatus::Rejected;
        }
      } else if (field == "echo_timeout_us" || field == "inter_sensor_gap_us") {
        if (command.numeric_value <= 0 || command.numeric_value > UINT32_MAX) {
          return CommandStatus::InvalidNumeric;
        }
        const auto value = static_cast<uint32_t>(command.numeric_value);
        if (field == "echo_timeout_us") {
          ultrasonic_echo_timeout_us_ = value;
        } else {
          ultrasonic_inter_sensor_gap_us_ = value;
        }
        ultrasonic_timing_set_ = ultrasonic_echo_timeout_us_ != 0U &&
                                 ultrasonic_inter_sensor_gap_us_ != 0U &&
                                 ultrasonic_tolerance_set_;
      } else if (field == "overshoot_tolerance_mm_milli") {
        if (command.numeric_value < 0 || command.numeric_value > UINT32_MAX) {
          return CommandStatus::InvalidNumeric;
        }
        ultrasonic_overshoot_tolerance_mm_milli_ =
            static_cast<uint32_t>(command.numeric_value);
        ultrasonic_tolerance_set_ = true;
        ultrasonic_timing_set_ = ultrasonic_echo_timeout_us_ != 0U &&
                                 ultrasonic_inter_sensor_gap_us_ != 0U &&
                                 ultrasonic_tolerance_set_;
      } else if (field == "reference_mv" || field == "full_scale_counts" ||
          field == "divider_numerator" || field == "divider_denominator") {
        if (command.numeric_value <= 0 || command.numeric_value > UINT32_MAX) {
          return CommandStatus::InvalidNumeric;
        }
        const auto value = static_cast<uint32_t>(command.numeric_value);
        if (field == "reference_mv") mq_reference_mv_ = value;
        else if (field == "full_scale_counts") mq_full_scale_counts_ = value;
        else if (field == "divider_numerator") mq_divider_numerator_ = value;
        else mq_divider_denominator_ = value;
        mq_engineering_set_ = mq_reference_mv_ != 0U &&
                              mq_full_scale_counts_ != 0U &&
                              mq_divider_numerator_ != 0U &&
                              mq_divider_denominator_ != 0U;
      } else if (field == "address") {
        const int64_t expected =
            pending_target_ == calibration::CalibrationTarget::Sht31_45 ? 0x45 : 0x44;
        if ((pending_target_ != calibration::CalibrationTarget::Sht31_44 &&
             pending_target_ != calibration::CalibrationTarget::Sht31_45) ||
            command.numeric_value != expected) {
          return CommandStatus::InvalidNumeric;
        }
      } else if (field == "samples") {
        if (command.numeric_value <= 0) return CommandStatus::InvalidNumeric;
        if (command.numeric_value > UINT16_MAX) return CommandStatus::InvalidNumeric;
        criteria_samples_ = static_cast<uint16_t>(command.numeric_value);
      } else if (field == "max_temperature_span_centi_c") {
        if (command.numeric_value <= 0) return CommandStatus::InvalidNumeric;
        if (command.numeric_value > std::numeric_limits<int32_t>::max()) {
          return CommandStatus::InvalidNumeric;
        }
        criteria_temperature_span_ = static_cast<int32_t>(command.numeric_value);
        criteria_temperature_span_set_ = true;
      } else if (field == "max_humidity_span_centi_pct") {
        if (command.numeric_value <= 0) return CommandStatus::InvalidNumeric;
        if (command.numeric_value > std::numeric_limits<int32_t>::max()) {
          return CommandStatus::InvalidNumeric;
        }
        criteria_humidity_span_ = static_cast<int32_t>(command.numeric_value);
        criteria_humidity_span_set_ = true;
      } else if (field == "max_span" || field == "max_span_counts") {
        if (command.numeric_value <= 0) return CommandStatus::InvalidNumeric;
        criteria_span_ = command.numeric_value;
        criteria_span_set_ = true;
      } else if (field == "reference_mass_mg") {
        if (command.numeric_value <= 0) return CommandStatus::InvalidNumeric;
        reference_mass_mg_ = static_cast<uint64_t>(command.numeric_value);
      } else {
        return CommandStatus::InvalidSyntax;
      }
      const bool is_sht = pending_target_ == calibration::CalibrationTarget::Sht31_44 ||
                          pending_target_ == calibration::CalibrationTarget::Sht31_45;
      if (criteria_samples_ == 0U ||
          (!is_sht && !criteria_span_set_) ||
          (is_sht && (!criteria_temperature_span_set_ ||
                      !criteria_humidity_span_set_))) {
        return CommandStatus::Accepted;
      }
      bool active_profile_present{false};
      if (profiles_ != nullptr) {
        const auto target = static_cast<acquisition::ProfileTarget>(
            static_cast<uint8_t>(pending_target_) + 1U);
        acquisition::ActiveProfileSnapshot active{};
        active_profile_present = profiles_->snapshot(
            acquisition::default_profile_key(target), active) && active.present;
      }
      if (pending_target_ >= calibration::CalibrationTarget::Hx711Organic &&
          pending_target_ <= calibration::CalibrationTarget::Hx711B3 &&
          reference_mass_mg_ == 0U) {
        return CommandStatus::Accepted;
      }
      if (is_ultrasonic_target(pending_target_) && !active_profile_present &&
          (!ultrasonic_echo_conversion_set_ || !ultrasonic_timing_set_)) {
        return CommandStatus::Accepted;
      }
      if (is_mq_target(pending_target_) && !active_profile_present &&
          !mq_engineering_set_) {
        return CommandStatus::Accepted;
      }
      if (pending_target_ == calibration::CalibrationTarget::Ds18b20B3 &&
          !active_profile_present && !ds_resolution_set_) {
        return CommandStatus::Accepted;
      }
      if (is_sht_target(pending_target_) && !active_profile_present &&
          (!reference_temperature_set_ || !reference_humidity_set_)) {
        return CommandStatus::Accepted;
      }
      // Criteria may be completed incrementally after the capture state has
      // been entered. Reference values and engineering fields remain session
      // owned; do not attempt to configure the CalibrationSession twice.
      if (criteria_set_) return CommandStatus::Accepted;
      if (pending_target_ >= calibration::CalibrationTarget::Mq4 &&
          pending_target_ <= calibration::CalibrationTarget::Mq8 &&
          criteria_span_ > std::numeric_limits<uint16_t>::max()) {
        return CommandStatus::InvalidNumeric;
      }
      if (pending_target_ == calibration::CalibrationTarget::Ds18b20B3 &&
          criteria_span_ > std::numeric_limits<int32_t>::max()) {
        return CommandStatus::InvalidNumeric;
      }
      calibration::CaptureCriteria criteria{calibration::Hx711CaptureCriteria{
          criteria_samples_, criteria_span_}};
      if (pending_target_ >= calibration::CalibrationTarget::UltrasonicOrganic &&
          pending_target_ <= calibration::CalibrationTarget::UltrasonicB3) {
        criteria = calibration::DistanceCaptureCriteria{criteria_samples_, static_cast<float>(criteria_span_)};
      } else if (pending_target_ >= calibration::CalibrationTarget::Mq4 &&
                 pending_target_ <= calibration::CalibrationTarget::Mq8) {
        criteria = calibration::MqCaptureCriteria{criteria_samples_,
                                                   static_cast<uint16_t>(criteria_span_),
                                                   pending_target_ == calibration::CalibrationTarget::Mq7};
      } else if (pending_target_ == calibration::CalibrationTarget::Ds18b20B3) {
        criteria = calibration::Ds18b20CaptureCriteria{criteria_samples_,
                                                        static_cast<int32_t>(criteria_span_), true};
      } else if (pending_target_ == calibration::CalibrationTarget::Sht31_44 ||
                 pending_target_ == calibration::CalibrationTarget::Sht31_45) {
        criteria = calibration::Sht31CaptureCriteria{criteria_samples_,
                                                     criteria_temperature_span_,
                                                     criteria_humidity_span_};
      }
      if (!calibration_.configure(criteria, now_us)) return CommandStatus::Rejected;
      criteria_set_ = true;
      return CommandStatus::Accepted;
    }
    case CommandKind::CalibrationStatus:
      if (!response.append("calibration_active=") ||
          !response.append((calibration_.state() == calibration::CalibrationState::Idle ||
                            calibration_.state() == calibration::CalibrationState::Failed)
                               ? "false\n" : "true\n")) return CommandStatus::Failed;
      return CommandStatus::Accepted;
    case CommandKind::CalibrationCapture: {
      if (!criteria_set_ || evidence_ == nullptr || command.has_numeric) return CommandStatus::Rejected;
      calibration::TrialObservation trial{};
      uint64_t sequence{0U};
      if (!evidence_->latest(pending_target_, evidence_sequence_, trial, sequence)) {
        return CommandStatus::Rejected;
      }
      if (pending_target_ == calibration::CalibrationTarget::Ds18b20B3) {
        const auto* ds_trial = std::get_if<calibration::Ds18b20Trial>(&trial);
        if (ds_trial == nullptr || ds_trial->resolution_bits < 9U ||
            ds_trial->resolution_bits > 12U ||
            (ds_resolution_set_ &&
             ds_trial->resolution_bits != ds_resolution_bits_)) {
          return CommandStatus::Rejected;
        }
      }
      calibration::CalibrationAnchor anchor = calibration::CalibrationAnchor::Primary;
      if (command.target.view() == "tare") anchor = calibration::CalibrationAnchor::Tare;
      else if (command.target.view() == "reference") anchor = calibration::CalibrationAnchor::Reference;
      else if (command.target.view() == "empty") anchor = calibration::CalibrationAnchor::Empty;
      else if (command.target.view() == "full") anchor = calibration::CalibrationAnchor::Full;
      else return CommandStatus::InvalidSyntax;
      if (pending_target_ == calibration::CalibrationTarget::Mq135Gas) calibration_.touch(now_us);
      if (!calibration_.observe_anchor(trial, anchor)) return CommandStatus::Rejected;
      if (pending_target_ == calibration::CalibrationTarget::Ds18b20B3) {
        const auto* ds_trial = std::get_if<calibration::Ds18b20Trial>(&trial);
        if (ds_trial != nullptr && ds_trial->wire_evidence &&
            ds_trial->rom_match && ds_trial->crc_valid &&
            ds_trial->conversion_complete) {
          ds_rom_ = ds_trial->rom;
          ds_rom_set_ = true;
          if (ds_trial->resolution_bits >= 9U &&
              ds_trial->resolution_bits <= 12U) {
            ds_resolution_bits_ = ds_trial->resolution_bits;
            ds_resolution_set_ = true;
          }
        }
      }
      calibration_.touch(now_us);
      evidence_sequence_ = sequence;
      return CommandStatus::Accepted;
    }
    case CommandKind::CalibrationValidate:
      return calibration_.validate() ? CommandStatus::Accepted : CommandStatus::Rejected;
    case CommandKind::CalibrationPreview: {
      CalibrationProposal proposal{};
      const bool built = build_calibration_proposal(now_us, proposal);
      const bool previewed = built ? calibration_.preview() : false;
      if (!built || !previewed) {
        return CommandStatus::Rejected;
      }
      calibration_proposal_ = proposal;
      const bool appended = append_calibration_preview(response);
      return appended ? CommandStatus::Accepted : CommandStatus::Failed;
    }
    case CommandKind::CalibrationCommit:
      if (!calibration_.request_commit()) return CommandStatus::Rejected;
      if (!commit_calibration_proposal()) {
        (void)calibration_.complete_commit(false);
        clear_calibration_proposal();
        return CommandStatus::Rejected;
      }
      if (profiles_ == nullptr || !calibration_proposal_.present) {
        (void)calibration_.complete_commit(false);
        clear_calibration_proposal();
        return CommandStatus::Failed;
      }
      {
        acquisition::ActiveProfileSnapshot committed{};
        if (!profiles_->snapshot(calibration_proposal_.key, committed) ||
            !committed.present ||
            !append_u64(response, "profile_version=",
                        committed.ref.profile_version) ||
            !response.append("profile_hash=") ||
            !append_hex(response, committed.ref.hash.data(),
                        committed.ref.hash.size()) ||
            !response.append("\n")) {
          (void)calibration_.complete_commit(false);
          clear_calibration_proposal();
          return CommandStatus::Failed;
        }
      }
      (void)calibration_.complete_commit(true);
      clear_calibration_proposal();
      mode_ = DeviceMode::Maintenance;
      return CommandStatus::Accepted;
    case CommandKind::CalibrationCancel:
      if (!calibration_.cancel()) return CommandStatus::Rejected;
      clear_calibration_proposal();
      criteria_set_ = false;
      refresh_mode();
      return CommandStatus::Accepted;
    case CommandKind::ProfileStatus:
    case CommandKind::ProfileExport:
    case CommandKind::ProfileImport:
    case CommandKind::ProfileImportBegin:
    case CommandKind::ProfileImportChunk:
    case CommandKind::ProfileImportFinish:
    case CommandKind::ProfileValidate:
    case CommandKind::ProfilePreview:
    case CommandKind::ProfileCommit:
    case CommandKind::ProfileRollback:
    case CommandKind::ProfileReset: {
      if (profiles_ == nullptr) return CommandStatus::Rejected;
      if (command.kind == CommandKind::ProfileImportChunk) {
        if (!transfer_import_active_) return CommandStatus::Rejected;
        return profile_transfer(command, response);
      }
      acquisition::ProfileKey key{};
      if (!profile_key_from_text(command.target, key)) return CommandStatus::InvalidSyntax;
      if (transfer_draft_active_ && key == transfer_key_) {
        transfer_last_activity_us_ = now_us;
      }
      if (command.kind == CommandKind::ProfileExport ||
          command.kind == CommandKind::ProfileImportBegin ||
          command.kind == CommandKind::ProfileImportFinish) {
        return profile_transfer(command, response);
      }
      if (command.kind == CommandKind::ProfileStatus) {
        const auto state = profiles_->state(key);
        if (!response.append("profile_state=") ||
            !response.append(state == acquisition::ProfileManagerState::Active ? "ACTIVE" :
                             state == acquisition::ProfileManagerState::Draft ? "DRAFT" :
                             state == acquisition::ProfileManagerState::Preview ? "PREVIEW" :
                             state == acquisition::ProfileManagerState::Error ? "ERROR" : "MISSING") ||
            !response.append("\n")) return CommandStatus::Failed;
        acquisition::ActiveProfileSnapshot snapshot{};
        if (!profiles_->snapshot(key, snapshot)) return CommandStatus::Failed;
        if (snapshot.present) {
          if (!append_u64(response, "profile_version=", snapshot.ref.profile_version) ||
              !response.append("profile_hash_present=true\n")) return CommandStatus::Failed;
        }
        return CommandStatus::Accepted;
      }
      if (command.kind == CommandKind::ProfileValidate) {
        return profiles_->validate(key) ? CommandStatus::Accepted : CommandStatus::Rejected;
      }
      if (command.kind == CommandKind::ProfilePreview) {
        return profiles_->preview(key) ? CommandStatus::Accepted : CommandStatus::Rejected;
      }
      if (command.kind == CommandKind::ProfileCommit) {
        if (!profiles_->commit(key)) return CommandStatus::Rejected;
        if (transfer_draft_active_ && key == transfer_key_) {
          transfer_draft_active_ = false;
          transfer_profile_ = acquisition::EncodedProfile{};
        }
        return CommandStatus::Accepted;
      }
      if (command.kind == CommandKind::ProfileRollback) {
        if (!profiles_->rollback(key)) return CommandStatus::Rejected;
        if (transfer_draft_active_ && key == transfer_key_) cancel_profile_transfer();
        return CommandStatus::Accepted;
      }
      if (command.kind == CommandKind::ProfileReset) {
        if (!profiles_->reset(key)) return CommandStatus::Rejected;
        if (transfer_draft_active_ && key == transfer_key_) cancel_profile_transfer();
        return CommandStatus::Accepted;
      }
      return CommandStatus::Rejected;
    }
    case CommandKind::ModelStatus:
    case CommandKind::ModelExport:
    case CommandKind::ModelImportBegin:
    case CommandKind::ModelImportChunk:
    case CommandKind::ModelImportFinish:
    case CommandKind::ModelValidate:
    case CommandKind::ModelPreview:
    case CommandKind::ModelCommit:
    case CommandKind::ModelCancel:
    case CommandKind::ModelRollback:
      return models_ == nullptr
                 ? CommandStatus::Rejected
                 : models_->execute_model(command, now_us, response);
    case CommandKind::StorageSdTest:
      return storage_probe_ == nullptr
                 ? CommandStatus::Rejected
                 : storage_probe_->run_sd_probe(response);
    case CommandKind::Reboot:
      calibration_.abort();
      clear_calibration_proposal();
      criteria_set_ = false;
      cancel_profile_transfer();
      if (models_ != nullptr) models_->cancel_model();
      mode_ = DeviceMode::RebootRequired;
      reboot_requested_ = true;
      if (reboot_ != nullptr) reboot_->request_reboot();
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

CommandStatus CommandRouter::execute_calibration_internal(
    const MaintenanceCommand& command, uint64_t now_us,
    MaintenanceResponse& response) noexcept {
  const bool previous = internal_calibration_dispatch_;
  internal_calibration_dispatch_ = true;
  const CommandStatus status = execute(command, now_us, response);
  internal_calibration_dispatch_ = previous;
  return status;
}

}  // namespace sigap::maintenance
