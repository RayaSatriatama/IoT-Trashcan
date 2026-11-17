#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>
#include "debug/types.h"

#ifndef SIGAP_SENSOR_SIMULATION
#define SIGAP_SENSOR_SIMULATION 0
#endif
#if SIGAP_SENSOR_SIMULATION != 0 && SIGAP_SENSOR_SIMULATION != 1
#error "SIGAP_SENSOR_SIMULATION must be 0 or 1"
#endif

#ifndef SIGAP_PHYSICAL_REFERENCE_ONLY
#define SIGAP_PHYSICAL_REFERENCE_ONLY 0
#endif
#if SIGAP_PHYSICAL_REFERENCE_ONLY != 0 && SIGAP_PHYSICAL_REFERENCE_ONLY != 1
#error "SIGAP_PHYSICAL_REFERENCE_ONLY must be 0 or 1"
#endif
#if SIGAP_PHYSICAL_REFERENCE_ONLY && SIGAP_SENSOR_SIMULATION
#error "Physical reference mode cannot enable sensor simulation"
#endif

#ifndef SIGAP_PROTOTYPE_OPERATION
#define SIGAP_PROTOTYPE_OPERATION 0
#endif
#if SIGAP_PROTOTYPE_OPERATION != 0 && SIGAP_PROTOTYPE_OPERATION != 1
#error "SIGAP_PROTOTYPE_OPERATION must be 0 or 1"
#endif
#if SIGAP_PROTOTYPE_OPERATION && SIGAP_SENSOR_SIMULATION
#error "Prototype operation requires physical sensors"
#endif

#ifndef SIGAP_LIVE_NO_BACKLOG
#define SIGAP_LIVE_NO_BACKLOG SIGAP_PROTOTYPE_OPERATION
#endif
#if SIGAP_LIVE_NO_BACKLOG != 0 && SIGAP_LIVE_NO_BACKLOG != 1
#error "SIGAP_LIVE_NO_BACKLOG must be 0 or 1"
#endif
#if SIGAP_LIVE_NO_BACKLOG && (!SIGAP_PROTOTYPE_OPERATION || SIGAP_SENSOR_SIMULATION)
#error "Latest-only live delivery requires physical prototype operation"
#endif

namespace sigap::build {

inline constexpr bool kPrototypeOperation{SIGAP_PROTOTYPE_OPERATION == 1};
inline constexpr bool kPhysicalReferenceOnly{SIGAP_PHYSICAL_REFERENCE_ONLY == 1};
inline constexpr bool kSensorSimulation{SIGAP_SENSOR_SIMULATION == 1};
inline constexpr bool kLiveNoBacklog{SIGAP_LIVE_NO_BACKLOG == 1};
inline constexpr uint64_t kLiveSnapshotPeriodUs{1000000U};

#ifndef SIGAP_LIVE_ARCHIVE_SPLIT
#define SIGAP_LIVE_ARCHIVE_SPLIT 0
#endif
#if SIGAP_LIVE_ARCHIVE_SPLIT != 0 && SIGAP_LIVE_ARCHIVE_SPLIT != 1
#error "SIGAP_LIVE_ARCHIVE_SPLIT must be 0 or 1"
#endif
inline constexpr bool kLiveArchiveSplit{SIGAP_LIVE_ARCHIVE_SPLIT == 1};

#ifdef SIGAP_FULL_DEBUG_BUILD
inline constexpr debug::Level kDebugDefaultLevel{debug::Level::Debug};
#else
inline constexpr debug::Level kDebugDefaultLevel{debug::Level::Info};
#endif

struct DebugUploadConfig final {
  std::string_view endpoint{};
  std::string_view device_code{};

  constexpr bool available() const noexcept {
    constexpr std::string_view prefix = "https://";
    constexpr std::string_view route = "/api/device-debug/device/";
    constexpr std::string_view suffix = "/events";
    if (endpoint.size() <= prefix.size() || endpoint.substr(0U, prefix.size()) != prefix ||
        device_code.empty() || device_code.size() > 64U || endpoint.size() > 192U ||
        !valid_utf8(device_code) || endpoint.find_first_of("?#\\") != std::string_view::npos)
      return false;
    const auto authority_end = endpoint.find('/', prefix.size());
    if (authority_end == std::string_view::npos || authority_end == prefix.size()) return false;
    const auto authority = endpoint.substr(prefix.size(), authority_end - prefix.size());
    if (authority.find('@') != std::string_view::npos ||
        endpoint.substr(authority_end, route.size()) != route ||
        endpoint.size() < authority_end + route.size() + suffix.size() ||
        endpoint.substr(endpoint.size() - suffix.size()) != suffix)
      return false;
    for (const unsigned char byte : endpoint)
      if (byte <= 0x20U || byte == 0x7fU) return false;
    const auto end = endpoint.size() - suffix.size();
    std::size_t matched{};
    for (auto cursor = authority_end + route.size(); cursor < end; ++cursor) {
      auto byte = static_cast<unsigned char>(endpoint[cursor]);
      if (byte == '/') return false;
      if (byte == '%') {
        if (cursor + 2U >= end) return false;
        const auto high = hex(endpoint[cursor + 1U]);
        const auto low = hex(endpoint[cursor + 2U]);
        if (high < 0 || low < 0) return false;
        byte = static_cast<unsigned char>((high << 4) | low);
        cursor += 2U;
      }
      if (matched == device_code.size() ||
          byte != static_cast<unsigned char>(device_code[matched++])) return false;
    }
    return matched == device_code.size();
  }

  static constexpr DebugUploadConfig make(std::string_view endpoint,
                                          std::string_view device_code) noexcept {
    DebugUploadConfig config{endpoint, device_code};
    return config.available() ? config : DebugUploadConfig{};
  }

 private:
  static constexpr int hex(char c) noexcept {
    return c >= '0' && c <= '9' ? c - '0' :
           c >= 'a' && c <= 'f' ? c - 'a' + 10 :
           c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
  }
  static constexpr bool valid_utf8(std::string_view text) noexcept {
    std::size_t cursor{};
    while (cursor < text.size()) {
      const auto first = static_cast<unsigned char>(text[cursor++]);
      if (first < 0x80U) continue;
      const unsigned extra = first >= 0xc2U && first <= 0xdfU ? 1U :
          first >= 0xe0U && first <= 0xefU ? 2U :
          first >= 0xf0U && first <= 0xf4U ? 3U : 9U;
      if (extra == 9U || cursor + extra > text.size()) return false;
      uint32_t point = first & (extra == 1U ? 0x1fU : extra == 2U ? 0x0fU : 0x07U);
      for (unsigned i = 0U; i < extra; ++i) {
        const auto next = static_cast<unsigned char>(text[cursor++]);
        if ((next & 0xc0U) != 0x80U) return false;
        point = (point << 6U) | (next & 0x3fU);
      }
      if ((extra == 1U && point < 0x80U) || (extra == 2U && point < 0x800U) ||
          (extra == 3U && point < 0x10000U) || point > 0x10ffffU ||
          (point >= 0xd800U && point <= 0xdfffU)) return false;
    }
    return true;
  }
};

#ifdef SIGAP_DEBUG_ENDPOINT
inline constexpr std::string_view kDebugEndpoint{SIGAP_DEBUG_ENDPOINT};
#else
inline constexpr std::string_view kDebugEndpoint{};
#endif
#ifdef SIGAP_DEBUG_DEVICE_CODE
inline constexpr std::string_view kDebugDeviceCode{SIGAP_DEBUG_DEVICE_CODE};
#else
inline constexpr std::string_view kDebugDeviceCode{};
#endif
inline constexpr DebugUploadConfig kDebugUploadConfig =
    DebugUploadConfig::make(kDebugEndpoint, kDebugDeviceCode);

inline constexpr char kBoardProfile[] = "n16r8";
// Bounded source/build identity used by boot metadata. This is not a
// scientific model version and is never used as a calibration provenance.
#if SIGAP_SENSOR_SIMULATION
inline constexpr char kFirmwareBuildId[] = "sigap-task6-sensor-sim-v1";
#elif SIGAP_LIVE_NO_BACKLOG
inline constexpr char kFirmwareBuildId[] = "sigap-live-https-v35";
#elif SIGAP_PROTOTYPE_OPERATION
inline constexpr char kFirmwareBuildId[] = "sigap-prototype-physical4-v27";
#elif SIGAP_PHYSICAL_REFERENCE_ONLY && SIGAP_LIVE_ARCHIVE_SPLIT
inline constexpr char kFirmwareBuildId[] = "sigap-task6-physical4-v2";
#elif SIGAP_PHYSICAL_REFERENCE_ONLY
inline constexpr char kFirmwareBuildId[] = "sigap-task6-physical4-v1";
#else
inline constexpr char kFirmwareBuildId[] = "sigap-task6";
#endif
inline constexpr uint32_t kConfigSchemaVersion{1};
inline constexpr uint32_t kTelemetrySchemaVersion{3};
inline constexpr uint16_t kMaxRecordsPerBatch{128};
inline constexpr std::size_t kMaxSerializedRecordBytes{16U * 1024U};
inline constexpr std::size_t kMaxBatchBodyBytes{32U * 1024U};

inline constexpr uint32_t kNvsOffset{0x9000};
inline constexpr uint32_t kNvsSize{0x5000};
inline constexpr uint32_t kPhyInitOffset{0xe000};
inline constexpr uint32_t kPhyInitSize{0x1000};
inline constexpr uint32_t kFactoryOffset{0x10000};
inline constexpr uint32_t kFactorySize{0x400000};
inline constexpr uint32_t kLittlefsOffset{0x410000};
inline constexpr uint32_t kLittlefsPartitionBytes{0x800000};
inline constexpr uint64_t kLittlefsReserveBytes{2ULL * 1024ULL * 1024ULL};
// Conservative pre-mount value supplied by the composition root. Runtime
// storage measurement belongs to the future filesystem/storage layer.
inline constexpr uint64_t kPreMountLittlefsValidationBytes{
    kLittlefsPartitionBytes};

constexpr bool partitions_fit_16mb() {
  return kNvsOffset + kNvsSize <= kPhyInitOffset &&
         kPhyInitOffset + kPhyInitSize <= kFactoryOffset &&
         kFactoryOffset + kFactorySize <= kLittlefsOffset &&
         kLittlefsOffset + kLittlefsPartitionBytes <= 16U * 1024U * 1024U;
}

static_assert(partitions_fit_16mb(), "SIGAP partitions must fit 16 MiB flash");

}  // namespace sigap::build
