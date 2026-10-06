#pragma once

#include <array>
#include <cstdint>

#include "config/config_types.h"

namespace sigap::acquisition {

enum class SensorId : uint8_t {
  Mq4, Mq135, Mq7, Mq8,
  Hx711Organic, Hx711Inorganic, Hx711B3,
  UltrasonicOrganic, UltrasonicInorganic, UltrasonicB3,
  Ds18b20B3, Sht31_44, Sht31_45,
};

// Profile families are part of persisted identity. A target alone is
// insufficient for channels such as MQ-7, which has independent ADC and
// heater profiles.
enum class ProfileKind : uint8_t {
  Calibration = 1,
  Adc = 2,
  Fis = 3,
  Safety = 4,
  Heater = 5,
  Capacity = 6,
};

enum class StreamId : uint8_t { Organic, Inorganic, B3, Ambient };
enum class SampleKind : uint8_t { MqAdc, Hx711Counts, UltrasonicEcho, Ds18b20, Sht31 };
enum class SampleQuality : uint8_t {
  Ready, WarmingUp, HeaterPhase, Uncalibrated, Calibrating,
  Timeout, Disconnected, CrcError, Stale, OutOfRange,
};
enum class SampleReason : uint8_t {
  None, DataReady, NotReady, Timeout, Disconnected, CrcError,
  InvalidProfile, HeaterPhase, ConversionPending, OutOfRange, ProfileMissing,
};
enum class TimeQuality : uint8_t { Unsynced, Synchronized, Estimated };
enum class ConvertedKind : uint8_t { None, Kilograms, FillPercent, AdcMillivolts, Celsius, HumidityPercent };
enum class DriverPollStatus : uint8_t { NoAttempt, Produced, Fault };

struct ConfigRef {
  uint32_t schema_version{0};
  uint64_t config_version{0};
  config::ConfigHash hash{};
  bool present{false};
};

struct ProfileRef {
  ProfileKind kind{ProfileKind::Calibration};
  uint32_t schema_version{0};
  uint32_t profile_version{0};
  uint32_t target_id{0};
  config::ConfigHash hash{};
  bool present{false};
};

struct DeviceId {
  std::array<uint8_t, 16> bytes{};
  bool present{false};
};

struct BootId {
  std::array<uint8_t, 16> bytes{};
  bool present{false};
};

struct RecordedAt {
  bool available{false};
  int64_t epoch_ms{0};
};

struct RawObservation {
  bool present{false};
  bool crc_valid{false};
  bool conversion_complete{false};
  bool identity_valid{false};
  bool temperature_crc_valid{false};
  bool humidity_crc_valid{false};
  bool heater_ready{false};
  int64_t signed_counts{0};
  uint16_t adc_counts{0};
  uint32_t distance_us{0};
  int32_t temperature_centi_c{0};
  int32_t humidity_centi_pct{0};
};

struct ConvertedObservation {
  bool primary_available{false};
  float primary_value{0.0F};
  ConvertedKind primary_kind{ConvertedKind::None};
  bool secondary_available{false};
  float secondary_value{0.0F};
  ConvertedKind secondary_kind{ConvertedKind::None};
};

struct SampleHeader {
  SensorId sensor{SensorId::Mq4};
  StreamId stream{StreamId::Organic};
  SampleKind kind{SampleKind::MqAdc};
  DeviceId device_id{};
  BootId boot_id{};
  uint64_t sequence{0};
  uint64_t monotonic_us{0};
  RecordedAt recorded_at{};
  TimeQuality time_quality{TimeQuality::Unsynced};
  ConfigRef config_ref{};
  ProfileRef profile_ref{};
};

struct SampleRecord {
  SampleHeader header{};
  SampleQuality quality{SampleQuality::Timeout};
  SampleReason reason{SampleReason::None};
  RawObservation raw{};
  ConvertedObservation converted{};
  bool gas_ppm_available{false};
};

}  // namespace sigap::acquisition
