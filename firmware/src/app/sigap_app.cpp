#include "sigap_app.h"

#include "sigap/board/board_memory.h"
#include "sigap/build/build_config.h"
#include "acquisition/provenance.h"
#include "acquisition/arduino_adapters.h"

#ifdef ARDUINO
#include <Arduino.h>
#endif

namespace sigap {

SigapApp::SigapApp() noexcept
    : config_manager_(config_store_,
                      config::ValidationContext{
                      build::kPreMountLittlefsValidationBytes, 0}),
      record_context_{boot_identity_.boot_id, acquisition::DeviceId{},
                      acquisition::ConfigRef{}, nullptr},
      record_factory_(boot_identity_.sequence, record_context_),
      profile_persistence_(profile_store_),
      profile_manager_(profile_persistence_),
      acquisition_(config_manager_, record_factory_, board::kN16R8Pins,
                   profiles_),
      maintenance_router_(config_manager_, secret_store_, &profile_manager_),
      maintenance_service_(maintenance_transport_, maintenance_router_) {
  profile_manager_.bind_runtime_profiles(profiles_);
}

// cppcheck-suppress unusedFunction
void SigapApp::setup() noexcept {
#ifdef ARDUINO
  Serial.begin(115200);
  board::log_runtime_board_memory();
#endif
  const config::ConfigBootResult boot_result = config_manager_.begin();
  (void)boot_result;
  for (uint32_t target = 1U; target <= 13U; ++target) {
    (void)profile_manager_.load(
        static_cast<acquisition::ProfileTarget>(target));
  }
  profile_generation_ = profile_manager_.generation();
  maintenance_router_.refresh_mode();
  if (!config_manager_.runtime_ready()) {
    acquisition_.disable();
    return;
  }
  const acquisition::ConfigRef ref =
      acquisition::active_config_ref(config_manager_);
  if (!ref.present) {
    acquisition_.disable();
    return;
  }
#ifdef ARDUINO
  const acquisition::ArduinoMonotonicClock clock{};
  (void)acquisition_.begin(config_manager_.active(), ref, clock.now_us());
#else
  (void)acquisition_.begin(config_manager_.active(), ref, 0U);
#endif
}

// cppcheck-suppress unusedFunction
void SigapApp::loop() noexcept {
#ifdef ARDUINO
  const acquisition::ArduinoMonotonicClock clock{};
  const uint64_t now_us = clock.now_us();
  maintenance_service_.poll(now_us);
  if (profile_generation_ != profile_manager_.generation()) {
    profile_generation_ = profile_manager_.generation();
    (void)acquisition_.refresh_profiles(now_us);
  }
  acquisition_.poll(now_us);
  maintenance_service_.poll(now_us);
#endif
}

}  // namespace sigap
