#include "maintenance/runtime.h"

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <cstring>
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
  const auto separator = value.find(':');
  FixedText<64U> base{};
  if (!base.assign(value.substr(0U, separator == std::string_view::npos
                                      ? value.size() : separator))) return false;
  if (!profile_target_from_text(base, key.target)) return false;
  key.kind = acquisition::default_profile_kind(key.target);
  if (separator != std::string_view::npos) {
    if (value.substr(separator + 1U) == "heater") {
      key.kind = acquisition::ProfileKind::Heater;
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

void set_calibration_provenance(acquisition::ProfileProvenance& provenance,
                                const acquisition::BootId* boot_id,
                                uint64_t now_us,
                                int64_t primary,
                                int64_t secondary,
                                uint32_t evidence_count,
                                uint64_t evidence_sequence,
                                uint8_t units) noexcept {
  provenance = acquisition::ProfileProvenance{};
  provenance.present = true;
  if (boot_id != nullptr) provenance.boot_id = *boot_id;
  provenance.monotonic_us = now_us;
  provenance.time_quality = acquisition::TimeQuality::Unsynced;
  provenance.reference_primary = primary;
  provenance.reference_secondary = secondary;
  provenance.evidence_count = evidence_count;
  provenance.evidence_sequence = evidence_sequence;
  provenance.units = units;
  constexpr char note[] = "calibration-evidence";
  std::memcpy(provenance.reference_note.data(), note, sizeof(note) - 1U);
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
    criteria_set_ = false;
    evidence_sequence_ = 0U;
    mode_ = DeviceMode::Maintenance;
  }
  if ((transfer_import_active_ || transfer_export_active_ || transfer_draft_active_) &&
      now_us >= transfer_last_activity_us_ &&
      now_us - transfer_last_activity_us_ > kTransferTimeoutUs) {
    cancel_profile_transfer();
  }
}

void CommandRouter::transport_lost() noexcept {
  calibration_.abort();
  criteria_set_ = false;
  evidence_sequence_ = 0U;
  cancel_profile_transfer();
  refresh_mode();
}

bool CommandRouter::build_and_stage_calibration_profile(uint64_t now_us) noexcept {
  if (profiles_ == nullptr) return false;
  const auto target = static_cast<acquisition::ProfileTarget>(
      static_cast<uint8_t>(pending_target_) + 1U);
  const acquisition::ProfileKey key{
      (pending_target_ >= calibration::CalibrationTarget::Mq4 &&
       pending_target_ <= calibration::CalibrationTarget::Mq8)
          ? acquisition::ProfileKind::Adc
          : acquisition::ProfileKind::Calibration,
      target};
  acquisition::ActiveProfileSnapshot active{};
  if (!profiles_->snapshot(key, active)) return false;
  if (active.present && active.ref.profile_version == UINT32_MAX) return false;
  const uint32_t version = active.present ? active.ref.profile_version + 1U : 1U;
  acquisition::ProfileCodec codec{};
  acquisition::EncodedProfile encoded{};
  switch (pending_target_) {
    case calibration::CalibrationTarget::Hx711Organic:
    case calibration::CalibrationTarget::Hx711Inorganic:
    case calibration::CalibrationTarget::Hx711B3: {
      if (reference_mass_mg_ == 0U || calibration_.secondary_stats().count == 0U) return false;
      acquisition::Hx711Profile profile{};
      profile.manifest.kind = acquisition::ProfileKind::Calibration;
      profile.manifest.profile_version = version;
      profile.manifest.target_id = static_cast<uint32_t>(target);
      profile.manifest.validity = acquisition::ProfileValidity::Valid;
      profile.tare_offset_counts = static_cast<int64_t>(calibration_.stats().mean);
      const double kg = static_cast<double>(reference_mass_mg_) / 1000000.0;
      const double delta = calibration_.secondary_stats().mean - calibration_.stats().mean;
      profile.counts_per_kg = static_cast<float>(delta / kg);
      set_calibration_provenance(profile.manifest.provenance, boot_id_, now_us,
                                 static_cast<int64_t>(profile.tare_offset_counts),
                                 static_cast<int64_t>(reference_mass_mg_),
                                 calibration_.stats().count +
                                     calibration_.secondary_stats().count,
                                 evidence_sequence_, 1U);
      if (!profile.valid_for_conversion() || !codec.encode(profile, encoded)) return false;
      break;
    }
    case calibration::CalibrationTarget::UltrasonicOrganic:
    case calibration::CalibrationTarget::UltrasonicInorganic:
    case calibration::CalibrationTarget::UltrasonicB3: {
      if (!active.present || calibration_.secondary_stats().count == 0U) return false;
      acquisition::UltrasonicProfile profile{};
      if (!codec.decode(active.encoded, profile)) return false;
      profile.manifest.profile_version = version;
      profile.d_empty_mm = static_cast<float>(calibration_.stats().mean);
      profile.d_full_mm = static_cast<float>(calibration_.secondary_stats().mean);
      profile.manifest.validity = acquisition::ProfileValidity::Valid;
      set_calibration_provenance(profile.manifest.provenance, boot_id_, now_us,
                                 static_cast<int64_t>(calibration_.stats().mean),
                                 static_cast<int64_t>(calibration_.secondary_stats().mean),
                                 calibration_.stats().count +
                                     calibration_.secondary_stats().count,
                                 evidence_sequence_, 2U);
      if (!profile.valid_for_conversion() || !codec.encode(profile, encoded)) return false;
      break;
    }
    default:
      if (!active.present) return false;
      if (pending_target_ == calibration::CalibrationTarget::Ds18b20B3) {
        if (!reference_temperature_set_) return false;
        acquisition::Ds18b20Profile profile{};
        if (!codec.decode(active.encoded, profile)) return false;
        profile.manifest.profile_version = version;
        profile.offset_centi_c = reference_temperature_centi_c_ -
                                 static_cast<int32_t>(calibration_.stats().mean);
        set_calibration_provenance(profile.manifest.provenance, boot_id_, now_us,
                                   reference_temperature_centi_c_, 0,
                                   calibration_.stats().count, evidence_sequence_, 3U);
        if (!profile.valid_for_conversion() || !codec.encode(profile, encoded)) return false;
      } else if (pending_target_ == calibration::CalibrationTarget::Sht31_44 ||
                 pending_target_ == calibration::CalibrationTarget::Sht31_45) {
        if (!reference_temperature_set_ || !reference_humidity_set_) return false;
        acquisition::Sht31Profile profile{};
        if (!codec.decode(active.encoded, profile)) return false;
        profile.manifest.profile_version = version;
        profile.temperature_offset_centi_c =
            reference_temperature_centi_c_ -
            static_cast<int32_t>(calibration_.stats().mean);
        profile.humidity_offset_centi_pct =
            reference_humidity_centi_pct_ -
            static_cast<int32_t>(calibration_.secondary_stats().mean);
        set_calibration_provenance(profile.manifest.provenance, boot_id_, now_us,
                                   reference_temperature_centi_c_,
                                   reference_humidity_centi_pct_,
                                   calibration_.stats().count +
                                       calibration_.secondary_stats().count,
                                   evidence_sequence_, 4U);
        if (!profile.valid_for_conversion() || !codec.encode(profile, encoded)) return false;
      } else {
        // MQ calibration retains validated ADC evidence without inventing a
        // gas model, ppm conversion, R0, or alarm threshold.
        acquisition::MqAdcProfile profile{};
        if (!codec.decode(active.encoded, profile)) return false;
        profile.manifest.profile_version = version;
        set_calibration_provenance(profile.manifest.provenance, boot_id_, now_us,
                                   static_cast<int64_t>(calibration_.stats().mean),
                                   0, calibration_.stats().count, evidence_sequence_, 5U);
        if (!profile.valid_for_conversion() || !codec.encode(profile, encoded)) return false;
      }
      break;
  }
  return profiles_->stage_import(key, encoded) && profiles_->validate(key) &&
         profiles_->preview(key) && profiles_->commit(key);
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
  const acquisition::ConfigRef current_config =
      config_ref_from_identity(config_.identity());
  if (audit_ != nullptr) audit_->set_context(now_us, current_config);
  if (profiles_ != nullptr) {
    profiles_->set_audit_context(now_us, current_config);
  }
  refresh_mode();
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
      if (!calibration_.begin(target, now_us)) return CommandStatus::Rejected;
      pending_target_ = target;
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
      mode_ = DeviceMode::Calibrating;
      return CommandStatus::Accepted;
    }
    case CommandKind::CalibrationCriteria: {
      const auto field = command.field.view();
      if (field == "reference_temperature_centi_c") {
        if (command.numeric_value < std::numeric_limits<int32_t>::min() ||
            command.numeric_value > std::numeric_limits<int32_t>::max()) {
          return CommandStatus::InvalidNumeric;
        }
        reference_temperature_centi_c_ = static_cast<int32_t>(command.numeric_value);
        reference_temperature_set_ = true;
        return CommandStatus::Accepted;
      }
      if (field == "reference_humidity_centi_pct") {
        if (command.numeric_value < 0 ||
            command.numeric_value > std::numeric_limits<int32_t>::max()) {
          return CommandStatus::InvalidNumeric;
        }
        reference_humidity_centi_pct_ = static_cast<int32_t>(command.numeric_value);
        reference_humidity_set_ = true;
        return CommandStatus::Accepted;
      }
      if (command.numeric_value <= 0) return CommandStatus::InvalidNumeric;
      if (field == "samples") {
        if (command.numeric_value > UINT16_MAX) return CommandStatus::InvalidNumeric;
        criteria_samples_ = static_cast<uint16_t>(command.numeric_value);
      } else if (field == "max_temperature_span_centi_c") {
        if (command.numeric_value > std::numeric_limits<int32_t>::max()) {
          return CommandStatus::InvalidNumeric;
        }
        criteria_temperature_span_ = static_cast<int32_t>(command.numeric_value);
        criteria_temperature_span_set_ = true;
      } else if (field == "max_humidity_span_centi_pct") {
        if (command.numeric_value > std::numeric_limits<int32_t>::max()) {
          return CommandStatus::InvalidNumeric;
        }
        criteria_humidity_span_ = static_cast<int32_t>(command.numeric_value);
        criteria_humidity_span_set_ = true;
      } else if (field == "max_span" || field == "max_span_counts") {
        criteria_span_ = command.numeric_value;
        criteria_span_set_ = true;
      } else if (field == "reference_mass_mg") {
        reference_mass_mg_ = static_cast<uint64_t>(command.numeric_value);
        return CommandStatus::Accepted;
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
      calibration::CalibrationAnchor anchor = calibration::CalibrationAnchor::Primary;
      if (command.target.view() == "tare") anchor = calibration::CalibrationAnchor::Tare;
      else if (command.target.view() == "reference") anchor = calibration::CalibrationAnchor::Reference;
      else if (command.target.view() == "empty") anchor = calibration::CalibrationAnchor::Empty;
      else if (command.target.view() == "full") anchor = calibration::CalibrationAnchor::Full;
      else return CommandStatus::InvalidSyntax;
      if (!calibration_.observe_anchor(trial, anchor)) return CommandStatus::Rejected;
      calibration_.touch(now_us);
      evidence_sequence_ = sequence;
      return CommandStatus::Accepted;
    }
    case CommandKind::CalibrationValidate:
      return calibration_.validate() ? CommandStatus::Accepted : CommandStatus::Rejected;
    case CommandKind::CalibrationPreview:
      return calibration_.preview() ? CommandStatus::Accepted : CommandStatus::Rejected;
    case CommandKind::CalibrationCommit:
      if (!calibration_.request_commit()) return CommandStatus::Rejected;
      if (!build_and_stage_calibration_profile(now_us)) {
        (void)calibration_.complete_commit(false);
        return CommandStatus::Rejected;
      }
      (void)calibration_.complete_commit(true);
      mode_ = DeviceMode::Maintenance;
      return CommandStatus::Accepted;
    case CommandKind::CalibrationCancel:
      if (!calibration_.cancel()) return CommandStatus::Rejected;
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
    case CommandKind::Reboot:
      calibration_.abort();
      criteria_set_ = false;
      cancel_profile_transfer();
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

}  // namespace sigap::maintenance
