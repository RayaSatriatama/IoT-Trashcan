#pragma once

// Wire-key parser for the normalized (dimensionless q0) MQ-135 criteria.
//
// The discriminator key `response_mode=normalized` selects this typed variant
// inside the existing mq135_gas target/session/controller. Absence of that key
// retains exact legacy criteria behaviour; an unknown/mixed key set is rejected
// before any mutation. No caller-supplied q0, RL, ADC coefficient or fitted
// constant is accepted, and `module_load_ohm` stays a legacy-only key.

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdint>
#include <string_view>

#include "acquisition/mq135_normalized.h"
#include "calibration/calibration_session.h"

namespace sigap::calibration {

inline constexpr std::string_view kMq135NormalizedResponseMode{"normalized"};

// Keys that only exist in the normalized contract. Seeing one without a prior
// response_mode=normalized is a mixed-mode request and must be rejected.
inline constexpr std::array<std::string_view, 9> kMq135NormalizedOnlyKeys{{
    "reference_kind", "preparation_confirmed", "preparation_method",
    "divider_top_ohm", "divider_bottom_ohm", "topology", "hardware_confirmed",
    "hardware_identity", "reference_environment"}};

inline bool mq135_normalized_only_key(std::string_view key) noexcept {
  for (const auto candidate : kMq135NormalizedOnlyKeys) {
    if (candidate == key) return true;
  }
  return false;
}

// Optional environment keys are accepted after the required set is complete and
// carry their own independent presence flags.
inline bool mq135_normalized_optional_environment(Mq135NormalizedCaptureCriteria& criteria,
                                                  std::string_view key,
                                                  int64_t value) noexcept {
  if (key == "reference_temperature_centi_c") {
    if (value < INT32_MIN || value > INT32_MAX) return false;
    criteria.temperature_centi_c = static_cast<int32_t>(value);
    criteria.temperature_present = true;
    return true;
  }
  if (key == "reference_humidity_centi_pct") {
    if (value < 0 || value > 10000) return false;
    criteria.humidity_centi_pct = static_cast<uint32_t>(value);
    criteria.humidity_present = true;
    return true;
  }
  return false;
}

class Mq135NormalizedParameters final {
 public:
  enum class Status : uint8_t {
    Incomplete,
    Complete,
    Duplicate,
    UnknownKey,
    MixedMode,
    InvalidValue,
  };

  Status set(std::string_view key, std::string_view value) noexcept {
    // A completed set still accepts the optional declared-environment keys and
    // still detects duplicate/unknown keys; only a hard error is sticky.
    if (issues_ != Status::Incomplete && issues_ != Status::Complete) return issues_;

    auto& c = criteria_;
    auto& e = c.electrical;
    if (key == "response_mode") {
      if (response_mode_seen_) return Status::Duplicate;
      if (value != kMq135NormalizedResponseMode) return Status::InvalidValue;
      response_mode_seen_ = true;
      return Status::Incomplete;
    }
    // Legacy-only key in normalized mode is a mixed-mode request.
    if (key == "module_load_ohm") return issues_ = Status::MixedMode;
    if (!response_mode_seen_) {
      // No normalized key may precede its discriminator.
      return issues_ = Status::MixedMode;
    }
    // Optional declared-environment keys are accepted once the required set is
    // complete and keep their own independent presence flags.
    if (complete() &&
        (key == "reference_temperature_centi_c" ||
         key == "reference_humidity_centi_pct")) {
      const bool temperature = key == "reference_temperature_centi_c";
      bool& seen = temperature ? optional_temperature_seen_ : optional_humidity_seen_;
      if (seen) return issues_ = Status::Duplicate;
      int64_t number{};
      if (!integer(value, number)) return issues_ = Status::InvalidValue;
      if (!mq135_normalized_optional_environment(criteria_, key, number)) {
        return issues_ = Status::InvalidValue;
      }
      seen = true;
      return issues_ = Status::Complete;
    }
    if (!known_key(key)) return issues_ = Status::UnknownKey;
    if (present_ & key_bit(key)) return issues_ = Status::Duplicate;

    bool ok = true;
    if (key == "hardware_identity" || key == "reference_environment") {
      if (!label(value)) return issues_ = Status::InvalidValue;
      auto& out = key == "hardware_identity" ? c.hardware_identity
                                             : c.reference_environment;
      out.fill(0);
      std::copy(value.begin(), value.end(), out.begin());
    } else if (key == "reference_kind") {
      if (value == "operational") {
        c.reference_kind = acquisition::Mq135ReferenceKind::Operational;
      } else if (value == "clean_air_confirmed") {
        c.reference_kind = acquisition::Mq135ReferenceKind::CleanAirConfirmed;
      } else {
        return issues_ = Status::InvalidValue;
      }
    } else if (key == "preparation_method") {
      if (value == "unknown") {
        c.preparation_method = acquisition::Mq135PreparationMethod::Unknown;
      } else if (value == "operator_declared") {
        c.preparation_method = acquisition::Mq135PreparationMethod::OperatorDeclared;
      } else {
        return issues_ = Status::InvalidValue;
      }
    } else if (key == "topology") {
      int64_t number{};
      if (!integer(value, number) || number < 1 || number > 2) {
        return issues_ = Status::InvalidValue;
      }
      e.topology = number == 1 ? acquisition::Mq135Topology::DirectLoaded
                               : acquisition::Mq135Topology::Buffered;
    } else if (key == "hardware_confirmed" || key == "reference_confirmed" ||
               key == "preparation_confirmed") {
      if (value == "1" || value == "true") {
        ok = flag(key, true);
      } else if (value == "0" || value == "false") {
        ok = flag(key, false);
      } else {
        return issues_ = Status::InvalidValue;
      }
      if (!ok) return issues_ = Status::InvalidValue;
    } else {
      int64_t number{};
      if (!integer(value, number)) return issues_ = Status::InvalidValue;
      ok = numeric(key, number);
      if (!ok) return issues_ = Status::InvalidValue;
    }
    present_ |= key_bit(key);
    (void)e;
    return complete() ? Status::Complete : Status::Incomplete;
  }

  bool arguments(std::string_view values) noexcept {
    while (!values.empty()) {
      const auto end = values.find(' ');
      const auto token = values.substr(0, end);
      const auto equals = token.find('=');
      if (equals == std::string_view::npos) return false;
      const auto status = set(token.substr(0, equals), token.substr(equals + 1));
      if (status != Status::Incomplete && status != Status::Complete) return false;
      if (end == std::string_view::npos) break;
      values.remove_prefix(end + 1);
    }
    return complete();
  }

  bool complete() const noexcept {
    return response_mode_seen_ && present_ == ((1ULL << kKeyCount) - 1ULL);
  }
  Status status() const noexcept { return issues_; }
  const Mq135NormalizedCaptureCriteria& criteria() const noexcept { return criteria_; }

 private:
  static constexpr unsigned kKeyCount{16U};

  static bool known_key(std::string_view key) noexcept {
    return index(key) != kKeyCount;
  }
  static unsigned index(std::string_view key) noexcept {
    constexpr std::array<std::string_view, kKeyCount> keys{{
        "samples", "min_duration_ms", "max_age_ms",
        "max_variation_milli_percent", "circuit_mv", "divider_top_ohm",
        "divider_bottom_ohm", "topology", "hardware_confirmed",
        "reference_confirmed", "hardware_identity", "reference_environment",
        "reference_kind", "preparation_confirmed", "preparation_method",
        "heating_duration_us"}};
    for (unsigned i = 0; i < kKeyCount; ++i) {
      if (keys[i] == key) return i;
    }
    return kKeyCount;
  }
  static uint32_t key_bit(std::string_view key) noexcept {
    return 1U << index(key);
  }
  static bool label(std::string_view value) noexcept {
    if (value.empty() || value.size() > 31U) return false;
    for (const unsigned char ch : value) {
      if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
            (ch >= '0' && ch <= '9') || ch == '_' || ch == '-' || ch == '.' ||
            ch == ':')) {
        return false;
      }
    }
    return true;
  }
  static bool integer(std::string_view value, int64_t& out) noexcept {
    const auto parsed = std::from_chars(value.data(), value.data() + value.size(), out);
    return !value.empty() && parsed.ec == std::errc{} &&
           parsed.ptr == value.data() + value.size();
  }
  bool flag(std::string_view key, bool value) noexcept {
    auto& c = criteria_;
    if (key == "hardware_confirmed") c.electrical.hardware_confirmed = value;
    else if (key == "reference_confirmed") c.reference_confirmed = value;
    else c.preparation_confirmed = value;
    return true;
  }
  bool numeric(std::string_view key, int64_t number) noexcept {
    auto& c = criteria_;
    if (key == "samples") {
      if (number < 3 || number > 64) return false;
      c.required_samples = c.criteria.samples = static_cast<uint16_t>(number);
      return true;
    }
    if (key == "min_duration_ms") {
      if (number <= 0) return false;
      c.criteria.min_duration_us = static_cast<uint64_t>(number) * 1000ULL;
      return true;
    }
    if (key == "max_age_ms") {
      if (number <= 0) return false;
      c.criteria.max_age_us = static_cast<uint64_t>(number) * 1000ULL;
      return true;
    }
    if (key == "max_variation_milli_percent") {
      if (number <= 0 || number > 100000) return false;
      c.criteria.max_variation_percent = static_cast<float>(number) / 1000.0F;
      return true;
    }
    if (key == "circuit_mv") {
      if (number <= 0 || number > static_cast<int64_t>(UINT32_MAX)) return false;
      c.electrical.circuit_mv = static_cast<uint32_t>(number);
      return true;
    }
    if (key == "divider_top_ohm" || key == "divider_bottom_ohm") {
      if (number <= 0 || number > static_cast<int64_t>(UINT32_MAX)) return false;
      auto& target = key == "divider_top_ohm" ? c.electrical.divider_top_ohm
                                              : c.electrical.divider_bottom_ohm;
      target = static_cast<uint32_t>(number);
      return true;
    }
    if (key == "heating_duration_us") {
      if (number < 0) return false;
      c.heating_duration_us = static_cast<uint64_t>(number);
      return true;
    }
    return false;
  }

  Mq135NormalizedCaptureCriteria criteria_{};
  uint64_t present_{0U};
  bool response_mode_seen_{false};
  bool optional_temperature_seen_{false};
  bool optional_humidity_seen_{false};
  Status issues_{Status::Incomplete};
};

}  // namespace sigap::calibration
