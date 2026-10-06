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
    // A non-zero, profile-defined timing tuple is the minimum safe test or
    // production profile.  Unknown durations must never energize the rail.
    return preheat_us > 0U && high_clean_us > 0U && low_detect_us > 0U;
  }
};

enum class Mq7HeaterState : uint8_t { Preheat, HighClean, LowDetect, Fault };

class Mq7HeaterController final {
 public:
  Mq7HeaterController(IMq7HeaterRail& rail, const Mq7HeaterProfile& profile) noexcept
      : rail_(rail), profile_(profile) {}

  void start(uint64_t now_us) noexcept {
    ready_ = false;
    state_started_at_ = now_us;
    state_ = Mq7HeaterState::Fault;
    if (!profile_.valid()) return;
    if (!rail_.select_high()) return;
    state_ = Mq7HeaterState::Preheat;
  }

  void reset(uint64_t now_us) noexcept { start(now_us); }

  void set_profile(const Mq7HeaterProfile& profile) noexcept {
    profile_ = profile;
    if (!profile_.valid()) {
      ready_ = false;
      state_ = Mq7HeaterState::Fault;
    }
  }

  void tick(uint64_t now_us) noexcept {
    if (state_ == Mq7HeaterState::Fault || now_us < state_started_at_ || !profile_.valid()) return;
    const uint64_t elapsed = now_us - state_started_at_;
    switch (state_) {
      case Mq7HeaterState::Preheat:
        if (elapsed >= profile_.preheat_us) enter_high_clean(now_us);
        break;
      case Mq7HeaterState::HighClean:
        if (elapsed >= profile_.high_clean_us) enter_low_detect(now_us);
        break;
      case Mq7HeaterState::LowDetect:
        if (elapsed >= profile_.low_detect_us) enter_high_clean(now_us);
        break;
      case Mq7HeaterState::Fault:
        break;
    }
  }

  Mq7HeaterState state() const noexcept { return state_; }
  bool operational_ready() const noexcept { return ready_ && state_ == Mq7HeaterState::LowDetect; }

 private:
  void enter_high_clean(uint64_t now_us) noexcept {
    ready_ = false;
    if (!rail_.select_high()) {
      state_ = Mq7HeaterState::Fault;
      return;
    }
    state_ = Mq7HeaterState::HighClean;
    state_started_at_ = now_us;
  }

  void enter_low_detect(uint64_t now_us) noexcept {
    if (!rail_.select_low()) {
      state_ = Mq7HeaterState::Fault;
      ready_ = false;
      return;
    }
    state_ = Mq7HeaterState::LowDetect;
    state_started_at_ = now_us;
    ready_ = true;
  }

  IMq7HeaterRail& rail_;
  Mq7HeaterProfile profile_{};
  uint64_t state_started_at_{0};
  Mq7HeaterState state_{Mq7HeaterState::Fault};
  bool ready_{false};
};

}  // namespace sigap::acquisition
