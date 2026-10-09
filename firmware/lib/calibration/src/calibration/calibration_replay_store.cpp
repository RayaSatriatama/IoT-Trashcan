#include "calibration/calibration_replay_store.h"

#include <cstddef>
#include <type_traits>

#ifdef ARDUINO
#include <nvs.h>
#endif

namespace sigap::calibration {

#ifdef ARDUINO

uint32_t NvsCalibrationReplayStore::checksum(const Record& record) noexcept {
  const auto* bytes = reinterpret_cast<const uint8_t*>(&record);
  constexpr std::size_t size = offsetof(Record, checksum);
  uint32_t value{2166136261U};
  for (std::size_t index = 0U; index < size; ++index) {
    value ^= bytes[index];
    value *= 16777619U;
  }
  return value;
}

bool NvsCalibrationReplayStore::encode(
    const CalibrationCommandEnvelope& command,
    const CalibrationResultEnvelope& result, Record& output,
    bool prepared) noexcept {
  output = Record{};
  output.magic = kMagic;
  output.version = kVersion;
  output.size = sizeof(Record);
  output.request_id = command.request_id;
  output.device_id = command.device_id;
  output.principal_id = command.principal_id;
  output.session_id = command.session_id;
  output.expires_at_us = command.expires_at_us;
  output.protocol_version = command.protocol_version;
  output.payload_size = command.payload_size;
  output.transport = static_cast<uint8_t>(command.transport);
  output.operation = static_cast<uint8_t>(command.operation);
  output.target = static_cast<uint8_t>(command.target);
  output.anchor = static_cast<uint8_t>(command.anchor);
  output.flags = static_cast<uint8_t>((command.criteria_present ? 1U : 0U) |
                                      (command.expected_profile_present ? 2U : 0U) |
                                      (prepared ? 4U : 0U));
  output.expected_version = command.expected_profile.version;
  output.expected_hash = command.expected_profile.hash;
  output.payload_digest = command.payload_digest;
  output.result_status = static_cast<uint8_t>(result.status);
  output.result_profile_present = result.profile.present ? 1U : 0U;
  output.result_request_id = result.request_id;
  output.result_session_id = result.session_id;
  output.result_profile_version = result.profile.version;
  output.result_profile_hash = result.profile.hash;
  bool criteria_ok = true;
  if (command.criteria_present) {
    std::visit(
        [&](const auto& criteria) noexcept {
          using T = std::decay_t<decltype(criteria)>;
          if constexpr (std::is_same_v<T, Hx711CaptureCriteria>) {
            output.criteria_kind = 1U;
            output.criteria_samples = criteria.required_samples;
            output.criteria_first = criteria.max_span_counts;
          } else if constexpr (std::is_same_v<T, DistanceCaptureCriteria>) {
            output.criteria_kind = 2U;
            output.criteria_samples = criteria.required_samples;
            output.criteria_first = static_cast<int32_t>(criteria.max_span_mm);
          } else if constexpr (std::is_same_v<T, MqCaptureCriteria>) {
            output.criteria_kind = 3U;
            output.criteria_samples = criteria.required_samples;
            output.criteria_first = criteria.max_span_counts;
            output.criteria_flag = criteria.require_low_detect ? 1U : 0U;
          } else if constexpr (std::is_same_v<T, Ds18b20CaptureCriteria>) {
            output.criteria_kind = 4U;
            output.criteria_samples = criteria.required_samples;
            output.criteria_first = criteria.max_span_centi_c;
            output.criteria_flag = criteria.require_rom_match ? 1U : 0U;
          } else {
            output.criteria_kind = 5U;
            output.criteria_samples = criteria.required_samples;
            output.criteria_first = criteria.max_temperature_span_centi_c;
            output.criteria_second = criteria.max_humidity_span_centi_pct;
          }
        },
        command.criteria);
  }
  if (command.criteria_present && output.criteria_kind == 0U) criteria_ok = false;
  if (!criteria_ok) return false;
  output.checksum = checksum(output);
  return command.request_id != 0U &&
         (prepared || result.status == CalibrationCommandStatus::Committed);
}

bool NvsCalibrationReplayStore::decode(
    const Record& record, CalibrationCommandEnvelope& command,
    CalibrationResultEnvelope& result, bool require_prepared) noexcept {
  const bool prepared = (record.flags & 4U) != 0U;
  if (record.magic != kMagic || record.version != kVersion ||
      record.size != sizeof(Record) || record.checksum != checksum(record) ||
      record.request_id == 0U ||
      record.operation != static_cast<uint8_t>(CalibrationOperation::Commit) ||
      prepared != require_prepared ||
      (!prepared && record.result_status !=
                        static_cast<uint8_t>(CalibrationCommandStatus::Committed))) {
    return false;
  }
  command = CalibrationCommandEnvelope{};
  command.protocol_version = record.protocol_version;
  command.request_id = record.request_id;
  command.device_id = record.device_id;
  command.principal_id = record.principal_id;
  command.session_id = record.session_id;
  command.expires_at_us = record.expires_at_us;
  command.transport = static_cast<CalibrationTransport>(record.transport);
  command.operation = static_cast<CalibrationOperation>(record.operation);
  command.target = static_cast<CalibrationTarget>(record.target);
  command.anchor = static_cast<CalibrationAnchor>(record.anchor);
  command.payload_size = record.payload_size;
  command.payload_digest = record.payload_digest;
  command.criteria_present = (record.flags & 1U) != 0U;
  command.expected_profile_present = (record.flags & 2U) != 0U;
  command.expected_profile.present = command.expected_profile_present;
  command.expected_profile.version = record.expected_version;
  command.expected_profile.hash = record.expected_hash;
  if (command.criteria_present) {
    switch (record.criteria_kind) {
      case 1U:
        command.criteria = Hx711CaptureCriteria{
            record.criteria_samples, record.criteria_first};
        break;
      case 2U:
        command.criteria = DistanceCaptureCriteria{
            record.criteria_samples, static_cast<float>(record.criteria_first)};
        break;
      case 3U:
        command.criteria = MqCaptureCriteria{
            record.criteria_samples, static_cast<uint16_t>(record.criteria_first),
            record.criteria_flag != 0U};
        break;
      case 4U:
        command.criteria = Ds18b20CaptureCriteria{
            record.criteria_samples, record.criteria_first,
            record.criteria_flag != 0U};
        break;
      case 5U:
        command.criteria = Sht31CaptureCriteria{
            record.criteria_samples, record.criteria_first, record.criteria_second};
        break;
      default:
        return false;
    }
  }
  result = CalibrationResultEnvelope{};
  result.protocol_version = command.protocol_version;
  result.request_id = prepared ? command.request_id : record.result_request_id;
  result.session_id = prepared ? command.session_id : record.result_session_id;
  result.status = prepared ? CalibrationCommandStatus::Failed
                            : CalibrationCommandStatus::Committed;
  result.profile.present = !prepared && record.result_profile_present != 0U;
  result.profile.version = prepared ? 0U : record.result_profile_version;
  result.profile.hash = prepared ? std::array<uint8_t, 32U>{}
                                  : record.result_profile_hash;
  return prepared || result.profile.present;
}

bool NvsCalibrationReplayStore::load(
    uint64_t request_id, CalibrationCommandEnvelope& command,
    CalibrationResultEnvelope& result) const noexcept {
  if (request_id == 0U) return false;
  nvs_handle_t handle{};
  if (nvs_open(kNamespace, NVS_READONLY, &handle) != ESP_OK) return false;
  Record record{};
  std::size_t size = sizeof(record);
  const esp_err_t status = nvs_get_blob(handle, kKey, &record, &size);
  nvs_close(handle);
  return status == ESP_OK && size == sizeof(record) &&
         record.request_id == request_id && decode(record, command, result);
}

bool NvsCalibrationReplayStore::prepare(
    const CalibrationCommandEnvelope& command) noexcept {
  Record record{};
  CalibrationResultEnvelope empty{};
  if (!encode(command, empty, record, true)) return false;
  nvs_handle_t handle{};
  if (nvs_open(kNamespace, NVS_READWRITE, &handle) != ESP_OK) return false;
  Record existing{};
  std::size_t existing_size = sizeof(existing);
  const esp_err_t existing_status =
      nvs_get_blob(handle, kKey, &existing, &existing_size);
  if (existing_status == ESP_OK) {
    if (existing_size != sizeof(existing)) {
      nvs_close(handle);
      return false;
    }
    const bool existing_prepared = (existing.flags & 4U) != 0U;
    CalibrationCommandEnvelope existing_command{};
    CalibrationResultEnvelope existing_result{};
    const bool valid = decode(existing, existing_command, existing_result,
                              existing_prepared);
    if (!valid ||
        (existing_prepared &&
         !calibration_command_identity_equal(existing_command, command))) {
      nvs_close(handle);
      return false;
    }
    // A matching prepared intent is already durable. Rewriting it is
    // unnecessary and could create a second failure window.
    if (existing_prepared) {
      nvs_close(handle);
      return true;
    }
  } else if (existing_status != ESP_ERR_NVS_NOT_FOUND) {
    nvs_close(handle);
    return false;
  }
  const esp_err_t write = nvs_set_blob(handle, kKey, &record, sizeof(record));
  const esp_err_t committed = write == ESP_OK ? nvs_commit(handle) : write;
  nvs_close(handle);
  return committed == ESP_OK;
}

bool NvsCalibrationReplayStore::load_prepared(
    uint64_t request_id, CalibrationCommandEnvelope& command) const noexcept {
  if (request_id == 0U) return false;
  nvs_handle_t handle{};
  if (nvs_open(kNamespace, NVS_READONLY, &handle) != ESP_OK) return false;
  Record record{};
  std::size_t size = sizeof(record);
  const esp_err_t status = nvs_get_blob(handle, kKey, &record, &size);
  nvs_close(handle);
  if (status != ESP_OK || size != sizeof(record) ||
      record.request_id != request_id) {
    return false;
  }
  CalibrationResultEnvelope ignored{};
  return decode(record, command, ignored, true);
}

bool NvsCalibrationReplayStore::save(
    const CalibrationCommandEnvelope& command,
    const CalibrationResultEnvelope& result) noexcept {
  Record record{};
  if (!encode(command, result, record, false)) return false;
  nvs_handle_t handle{};
  if (nvs_open(kNamespace, NVS_READWRITE, &handle) != ESP_OK) return false;
  const esp_err_t write = nvs_set_blob(handle, kKey, &record, sizeof(record));
  const esp_err_t committed = write == ESP_OK ? nvs_commit(handle) : write;
  nvs_close(handle);
  return committed == ESP_OK;
}

#endif  // ARDUINO

}  // namespace sigap::calibration
