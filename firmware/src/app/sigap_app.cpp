#include "sigap_app.h"

#include "sigap/build/build_config.h"

namespace sigap {

SigapApp::SigapApp() noexcept
    : config_manager_(config_store_,
                      config::ValidationContext{
                          build::kLittlefsPartitionBytes, 0}) {}

// cppcheck-suppress unusedFunction
void SigapApp::setup() noexcept {
  // Foundation slice: load and validate the immutable per-boot snapshot.
  // Sensor, storage, transport, and maintenance services are later slices.
  (void)config_manager_.begin();
}

// cppcheck-suppress unusedFunction
void SigapApp::loop() noexcept {}

}  // namespace sigap
