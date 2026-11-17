#pragma once
#include <array>
#include <charconv>
#include <string_view>
#include "calibration/calibration_session.h"

namespace sigap::calibration {
// Protocol/storage bounds only. Stability limits and measured hardware values
// are mandatory operator input, not manufacturer-derived default thresholds.
inline constexpr std::array<std::string_view, 17> kMq135ReferenceParameterNames{{
  "samples", "min_duration_ms", "max_age_ms", "max_variation_milli_percent",
  "circuit_mv", "module_load_ohm", "divider_top_ohm", "divider_bottom_ohm",
  "topology", "hardware_confirmed", "preparation_confirmed", "reference_confirmed",
  "reference_temperature_centi_c", "reference_humidity_centi_pct", "heating_duration_us",
  "hardware_identity", "reference_environment"
}};
inline bool mq135_reference_label(std::string_view value) noexcept {
  if (value.empty() || value.size() > 31) return false;
  for (const unsigned char ch : value)
    if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
          (ch >= '0' && ch <= '9') || ch == '_' || ch == '-' || ch == '.' || ch == ':')) return false;
  return true;
}
class Mq135ReferenceParameters final {
 public:
  bool set(std::string_view key, std::string_view value) noexcept {
    std::size_t index = 0;
    while (index < kMq135ReferenceParameterNames.size() && kMq135ReferenceParameterNames[index] != key) ++index;
    if (index == kMq135ReferenceParameterNames.size() || (present_ & (1U << index))) return false;
    auto& p = criteria_.profile;
    if (index >= 15) {
      if (!mq135_reference_label(value)) return false;
      auto& out = index == 15 ? p.hardware_identity : p.reference_environment;
      out.fill(0); std::copy(value.begin(), value.end(), out.begin());
    } else {
      int64_t number{};
      const auto parsed = std::from_chars(value.data(), value.data() + value.size(), number);
      if (value.empty() || parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size()) return false;
      if (index == 12) {
        if (number < INT32_MIN || number > INT32_MAX) return false;
        p.temperature_centi_c = static_cast<int32_t>(number);
      } else if (index == 13) {
        if (number < 0 || number > 10000) return false;
        p.humidity_centi_percent = static_cast<uint32_t>(number);
      } else {
        if (number <= 0) return false;
        switch (index) {
          case 0:
            if (number < 3 || number > 64) return false;
            criteria_.required_samples = p.criteria.samples = static_cast<uint16_t>(number); break;
          case 1: case 2:
            if (number > INT64_MAX / 1000) return false;
            if (index == 1) p.criteria.min_duration_us = static_cast<uint64_t>(number) * 1000;
            else p.criteria.max_age_us = static_cast<uint64_t>(number) * 1000;
            break;
          case 3:
            if (number > UINT32_MAX) return false;
            p.criteria.max_variation_percent = static_cast<float>(number) / 1000; break;
          case 4: case 5: case 6: case 7:
            if (number > UINT32_MAX) return false;
            if (index == 4) p.electrical.circuit_mv = static_cast<float>(number);
            if (index == 5) p.electrical.module_load_ohm = static_cast<float>(number);
            if (index == 6) p.electrical.divider_top_ohm = static_cast<float>(number);
            if (index == 7) p.electrical.divider_bottom_ohm = static_cast<float>(number);
            break;
          case 8:
            if (number != 1 && number != 2) return false;
            p.electrical.topology = static_cast<acquisition::Mq135Topology>(number); break;
          case 9: case 10: case 11:
            if (number != 1) return false;
            if (index == 9) p.electrical.verified = true;
            if (index == 10) p.preparation_confirmed = true;
            if (index == 11) criteria_.reference_confirmed = true;
            break;
          case 14: p.heating_duration_us = static_cast<uint64_t>(number); break;
          default: return false;
        }
      }
    }
    present_ |= 1U << index;
    return true;
  }
  bool complete() const noexcept { return present_ == ((1U << kMq135ReferenceParameterNames.size()) - 1); }
  const Mq135GasCaptureCriteria& criteria() const noexcept { return criteria_; }
  bool arguments(std::string_view values) noexcept {
    while (!values.empty()) {
      const auto end = values.find(' ');
      const auto token = values.substr(0, end);
      const auto equals = token.find('=');
      if (equals == std::string_view::npos || !set(token.substr(0, equals), token.substr(equals + 1))) return false;
      if (end == std::string_view::npos) break;
      values.remove_prefix(end + 1);
    }
    return complete();
  }
 private:
  Mq135GasCaptureCriteria criteria_{};
  uint32_t present_{0};
};
} // namespace sigap::calibration
