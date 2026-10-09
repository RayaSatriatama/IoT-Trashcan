#pragma once

#include <array>
#include <cstdint>
#include <cstring>

#include "calibration/calibration_transport.h"

namespace sigap::calibration {

// Native tests use this store to model a restart without touching hardware.
class MemoryCalibrationReplayStore final
    : public ICalibrationTransportReplayStore {
 public:
  bool prepare(const CalibrationCommandEnvelope& command) noexcept override {
    if (fail_prepare || command.request_id == 0U) return false;
    if (prepared_ &&
        !calibration_command_identity_equal(command_, command)) {
      return false;
    }
    if (prepared_) return true;
    command_ = command;
    result_ = CalibrationResultEnvelope{};
    prepared_ = true;
    valid_ = false;
    return true;
  }

  bool load(uint64_t request_id, CalibrationCommandEnvelope& command,
            CalibrationResultEnvelope& result) const noexcept override {
    if (!valid_ || request_id == 0U || command_.request_id != request_id) {
      return false;
    }
    command = command_;
    result = result_;
    return true;
  }

  bool load_prepared(uint64_t request_id,
                    CalibrationCommandEnvelope& command) const noexcept override {
    if (!prepared_ || request_id == 0U || command_.request_id != request_id) {
      return false;
    }
    command = command_;
    return true;
  }

  bool save(const CalibrationCommandEnvelope& command,
            const CalibrationResultEnvelope& result) noexcept override {
    if (fail_save || command.request_id == 0U) return false;
    command_ = command;
    result_ = result;
    valid_ = true;
    prepared_ = false;
    return true;
  }

  void clear() noexcept {
    command_ = CalibrationCommandEnvelope{};
    result_ = CalibrationResultEnvelope{};
    valid_ = false;
    prepared_ = false;
  }

  bool fail_prepare{false};
  bool fail_save{false};

 private:
  CalibrationCommandEnvelope command_{};
  CalibrationResultEnvelope result_{};
  bool valid_{false};
  bool prepared_{false};
};

#ifdef ARDUINO

// The replay record contains only the bounded identity and the committed
// profile reference. It is independent from the profile payload and is kept
// in its own NVS namespace so a lost HTTP response can be reconciled after a
// reboot without repeating calibration side effects.
class NvsCalibrationReplayStore final
    : public ICalibrationTransportReplayStore {
 public:
  bool load(uint64_t request_id, CalibrationCommandEnvelope& command,
            CalibrationResultEnvelope& result) const noexcept override;
  bool prepare(const CalibrationCommandEnvelope& command) noexcept override;
  bool load_prepared(uint64_t request_id,
                    CalibrationCommandEnvelope& command) const noexcept override;
  bool save(const CalibrationCommandEnvelope& command,
            const CalibrationResultEnvelope& result) noexcept override;

 private:
  struct Record final {
    uint32_t magic{0U};
    uint16_t version{0U};
    uint16_t size{0U};
    uint64_t request_id{0U};
    uint64_t device_id{0U};
    uint64_t principal_id{0U};
    uint64_t session_id{0U};
    uint64_t expires_at_us{0U};
    uint16_t protocol_version{0U};
    uint16_t payload_size{0U};
    uint8_t transport{0U};
    uint8_t operation{0U};
    uint8_t target{0U};
    uint8_t anchor{0U};
    uint8_t flags{0U};
    uint8_t criteria_kind{0U};
    uint16_t criteria_samples{0U};
    int32_t criteria_first{0};
    int32_t criteria_second{0};
    uint8_t criteria_flag{0U};
    uint8_t reserved[3U]{};
    uint64_t expected_version{0U};
    std::array<uint8_t, 32U> expected_hash{};
    std::array<uint8_t, 32U> payload_digest{};
    uint8_t result_status{0U};
    uint8_t result_profile_present{0U};
    uint16_t result_reserved{0U};
    uint64_t result_request_id{0U};
    uint64_t result_session_id{0U};
    uint64_t result_profile_version{0U};
    std::array<uint8_t, 32U> result_profile_hash{};
    uint32_t checksum{0U};
  };

  static constexpr uint32_t kMagic{0x53435250U};  // "SCRP"
  static constexpr uint16_t kVersion{1U};
  inline static constexpr char kNamespace[] = "sigap_cal";
  inline static constexpr char kKey[] = "replay";

  static uint32_t checksum(const Record& record) noexcept;
  static bool encode(const CalibrationCommandEnvelope& command,
                     const CalibrationResultEnvelope& result,
                     Record& output, bool prepared = false) noexcept;
  static bool decode(const Record& record,
                     CalibrationCommandEnvelope& command,
                     CalibrationResultEnvelope& result,
                     bool require_prepared = false) noexcept;
};

#endif  // ARDUINO

}  // namespace sigap::calibration
