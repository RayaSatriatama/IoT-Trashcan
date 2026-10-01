#include "sigap_app.h"

#include "sigap/board/board_memory.h"
#include "sigap/build/build_config.h"

#ifdef ARDUINO
#include <Arduino.h>
#endif

namespace sigap {

SigapApp::SigapApp() noexcept
    : config_manager_(config_store_,
                      config::ValidationContext{
                          build::kPreMountLittlefsValidationBytes, 0}) {}

// cppcheck-suppress unusedFunction
void SigapApp::setup() noexcept {
#ifdef ARDUINO
  Serial.begin(115200);
  board::log_runtime_board_memory();
#endif
  // Foundation slice: load and validate the immutable per-boot snapshot.
  // Sensor, storage, transport, and maintenance services are later slices.
  (void)config_manager_.begin();
}

// cppcheck-suppress unusedFunction
void SigapApp::loop() noexcept {}

}  // namespace sigap
