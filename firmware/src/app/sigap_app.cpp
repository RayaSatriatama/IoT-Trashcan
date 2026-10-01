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
      acquisition_(config_manager_) {}

// cppcheck-suppress unusedFunction
void SigapApp::setup() noexcept {
#ifdef ARDUINO
  Serial.begin(115200);
  board::log_runtime_board_memory();
#endif
  // Foundation slice: load and validate the immutable per-boot snapshot.
  (void)config_manager_.begin();
  acquisition_.set_active_config(config_manager_.active(),
                                 acquisition::active_config_ref(config_manager_));
}

// cppcheck-suppress unusedFunction
void SigapApp::loop() noexcept {
#ifdef ARDUINO
  acquisition_.poll(static_cast<uint64_t>(micros()));
#endif
}

}  // namespace sigap
