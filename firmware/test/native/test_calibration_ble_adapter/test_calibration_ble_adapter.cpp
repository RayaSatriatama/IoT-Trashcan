#include <string_view>

#include <unity.h>

#include "maintenance/ble_calibration_backend.h"
#include "maintenance/runtime.h"
#include "config/config_store.h"
#include "config/config_manager.h"
#include "config/config_validator.h"
#include "provisioning/secret_store.h"

namespace {

using namespace sigap::maintenance;

constexpr sigap::config::ValidationContext kContext{8ULL * 1024ULL * 1024ULL,
                                                     0U};

class Clock final : public ICalibrationClock {
 public:
  uint64_t now_us() const noexcept override { return now; }
  uint64_t now{100U};
};

void test_ble_adapter_uses_authoritative_parser_and_router() {
  sigap::provisioning::BleCommand parsed{};
  TEST_ASSERT_EQUAL_INT(
      static_cast<int>(sigap::provisioning::BleParseStatus::Accepted),
      static_cast<int>(sigap::provisioning::BleCommandParser::parse(
          "CAL STATUS", parsed)));

  sigap::config::MemoryConfigStore config_store{};
  sigap::config::ConfigManager config(config_store, kContext);
  TEST_ASSERT_EQUAL_INT(
      static_cast<int>(sigap::config::ConfigBootResult::BootstrapDefaults),
      static_cast<int>(config.begin()));
  sigap::provisioning::MemorySecretBlobStore secret_store{};
  sigap::provisioning::PersistentSecretStore secrets(secret_store);
  CommandRouter router(config, secrets);
  Clock clock{};
  CommandRouterBleCalibrationBackend backend(router, clock);
  char response[192]{};
  std::size_t size{0U};

  TEST_ASSERT_TRUE(backend.execute("CAL STATUS", response, sizeof(response), size));
  TEST_ASSERT_TRUE(std::string_view(response, size).find(
                       "calibration_active=false") != std::string_view::npos);

  size = 0U;
  TEST_ASSERT_FALSE(backend.execute("CAL UNKNOWN", response, sizeof(response), size));
  size = 0U;
  TEST_ASSERT_TRUE(backend.execute("CAL START hx711_organic", response,
                                   sizeof(response), size));
  TEST_ASSERT_TRUE(backend.execute(
      "CAL CRITERIA hx711_organic samples=2 max_span_counts=10",
      response, sizeof(response), size));
  TEST_ASSERT_TRUE(backend.execute("CAL CAPTURE hx711_organic tare", response,
                                   sizeof(response), size) == false);
  // A BLE-owned session is the only session a BLE disconnect may cancel.
  backend.disconnect();
  size = 0U;
  TEST_ASSERT_TRUE(backend.execute("CAL STATUS", response, sizeof(response), size));
  TEST_ASSERT_TRUE(std::string_view(response, size).find(
                       "calibration_active=false") != std::string_view::npos);
}

}  // namespace

void setup() {
  UNITY_BEGIN();
  RUN_TEST(test_ble_adapter_uses_authoritative_parser_and_router);
  UNITY_END();
}

int main() {
  setup();
  return 0;
}
