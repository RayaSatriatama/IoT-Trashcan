#pragma once

#include <cstdint>

#include "acquisition/profiles.h"

namespace sigap::acquisition {

class IMq7HeaterRail {
 public:
  virtual ~IMq7HeaterRail() = default;
  virtual bool select_high() noexcept = 0;
  virtual bool select_low() noexcept = 0;
};

struct Mq7HeaterProfile : ProfileRef {
  uint64_t preheat_us{0};
  uint64_t high_clean_us{0};
  uint64_t low_detect_us{0};
  bool valid() const noexcept {
    return preheat_us > 0U && high_clean_us > 0U && low_detect_us > 0U;
  }
};

enum class Mq7HeaterState : uint8_t { Preheat, HighClean, LowDetect, Fault };

class Mq7HeaterController final {
 public:
  Mq7HeaterController(IMq7HeaterRail& rail, const Mq7HeaterProfile& profile) noexcept
      : rail_(rail), profile_(profile) {}

  void start(uint64_t now_us) noexcept {
    started_at_ = now_us;
    state_ = Mq7HeaterState::Preheat;
    ready_ = false;
    if (!profile_.valid() || !rail_.select_high()) state_ = Mq7HeaterState::Fault;
  }

  void reset(uint64_t now_us) noexcept { start(now_us); }

  void tick(uint64_t now_us) noexcept {
    if (state_ == Mq7HeaterState::Fault || now_us < started_at_ || !profile_.valid()) return;
    const uint64_t elapsed = now_us - started_at_;
    if (state_ == Mq7HeaterState::Preheat && elapsed >= profile_.preheat_us) {
      if (!rail_.select_high()) { state_ = Mq7HeaterState::Fault; return; }
      state_ = Mq7HeaterState::HighClean;
    }
    if (state_ == Mq7HeaterState::HighClean && elapsed >= profile_.preheat_us + profile_.high_clean_us) {
      if (!rail_.select_low()) { state_ = Mq7HeaterState::Fault; return; }
      state_ = Mq7HeaterState::LowDetect;
      ready_ = true;
    }
  }

  Mq7HeaterState state() const noexcept { return state_; }
  bool operational_ready() const noexcept { return ready_ && state_ == Mq7HeaterState::LowDetect; }

 private:
  IMq7HeaterRail& rail_;
  Mq7HeaterProfile profile_{};
  uint64_t started_at_{0};
  Mq7HeaterState state_{Mq7HeaterState::Fault};
  bool ready_{false};
};

}  // namespace sigap::acquisition
