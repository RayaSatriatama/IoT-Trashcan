#include <cstdint>
#include <string>
#include <string_view>
#include <variant>

#include <unity.h>

#include "acquisition/profile_manager.h"
#include "acquisition/profile_store.h"
#include "calibration/calibration_session.h"
#include "config/config_store.h"
#include "config/config_validator.h"
#include "maintenance/maintenance.h"
#include "maintenance/runtime.h"
#include "provisioning/secret_store.h"

namespace {

using namespace sigap;
using namespace sigap::maintenance;

constexpr config::ValidationContext kValidationContext{8ULL * 1024ULL * 1024ULL, 0U};

struct Evidence final : calibration::ICalibrationEvidenceSource {
  calibration::TrialObservation value{};
  uint64_t sequence{0U};

  bool latest(calibration::CalibrationTarget target, uint64_t after,
              calibration::TrialObservation& output,
              uint64_t& observed) const noexcept override {
    if (sequence == 0U || sequence <= after) return false;
    const bool matches = [&] {
      switch (target) {
        case calibration::CalibrationTarget::UltrasonicB3:
          return std::holds_alternative<calibration::UltrasonicTrial>(value);
        case calibration::CalibrationTarget::Mq4:
          return std::holds_alternative<calibration::MqTrial>(value);
        case calibration::CalibrationTarget::Ds18b20B3:
          return std::holds_alternative<calibration::Ds18b20Trial>(value);
        case calibration::CalibrationTarget::Sht31_44:
          return std::holds_alternative<calibration::Sht31Trial>(value);
        default:
          return false;
      }
    }();
    if (!matches) return false;
    output = value;
    observed = sequence;
    return true;
  }

  bool set_ultrasonic_engineering_conversion(
      calibration::CalibrationTarget, float value) noexcept override {
    return value > 0.0F;
  }
};

struct Fixture final {
  config::MemoryConfigStore config_store{};
  config::ConfigManager config{config_store, kValidationContext};
  provisioning::MemorySecretBlobStore secret_store{};
  provisioning::PersistentSecretStore secrets{secret_store};
  acquisition::MemoryProfileStore profile_store{};
  acquisition::ProfilePersistence persistence{profile_store};
  acquisition::ProfileCodec codec{};
  acquisition::ProfileManager profiles{persistence, codec};
  Evidence evidence{};
  CommandRouter router{config, secrets, &profiles, &evidence};

  Fixture() noexcept {
    (void)config.begin();
  }
};

CommandStatus command(CommandRouter& router, std::string_view text,
                      uint64_t now) {
  MaintenanceParser parser{};
  ParseEvent event{};
  bool emitted{false};
  for (const char byte : text) emitted = parser.consume(byte, event) || emitted;
  TEST_ASSERT_TRUE(emitted);
  MaintenanceResponse response{};
  return router.execute(event.command, now, response);
}

void start_common(Fixture& fixture, const char* target, const char* span,
                  const char* samples = "1") {
  TEST_ASSERT_EQUAL_INT(static_cast<int>(CommandStatus::Accepted),
                        static_cast<int>(command(fixture.router,
                                                  std::string("calibration start ") + target + "\n", 1U)));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(CommandStatus::Accepted),
                        static_cast<int>(command(fixture.router,
                                                  std::string("calibration criteria samples ") + samples + "\n", 2U)));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(CommandStatus::Accepted),
                        static_cast<int>(command(fixture.router,
                                                  std::string("calibration criteria max_span ") + span + "\n", 3U)));
}

void finish(Fixture& fixture, const char* anchor, uint64_t now) {
  TEST_ASSERT_EQUAL_INT(static_cast<int>(CommandStatus::Accepted),
                        static_cast<int>(command(fixture.router,
                                                  std::string("calibration capture ") + anchor + "\n", now)));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(CommandStatus::Accepted),
                        static_cast<int>(command(fixture.router,
                                                  "calibration validate\n", now + 1U)));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(CommandStatus::Accepted),
                        static_cast<int>(command(fixture.router, "calibration preview\n", now + 2U)));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(CommandStatus::Accepted),
                        static_cast<int>(command(fixture.router, "calibration commit\n", now + 3U)));
}

void test_first_time_ultrasonic_profile_is_typed_and_persistent() {
  Fixture fixture{};
  start_common(fixture, "ultrasonic_b3", "10");
  TEST_ASSERT_EQUAL_INT(0, static_cast<int>(command(fixture.router, "calibration criteria echo_mm_per_us_milli 171\n", 4U)));
  TEST_ASSERT_EQUAL_INT(0, static_cast<int>(command(fixture.router, "calibration criteria echo_timeout_us 30000\n", 5U)));
  TEST_ASSERT_EQUAL_INT(0, static_cast<int>(command(fixture.router, "calibration criteria inter_sensor_gap_us 60000\n", 6U)));
  TEST_ASSERT_EQUAL_INT(0, static_cast<int>(command(fixture.router, "calibration criteria overshoot_tolerance_mm_milli 0\n", 7U)));
  fixture.evidence.value = calibration::UltrasonicTrial{300.0F, acquisition::SampleQuality::Uncalibrated, true, true};
  fixture.evidence.sequence = 1U;
  TEST_ASSERT_EQUAL_INT(static_cast<int>(CommandStatus::Accepted),
                        static_cast<int>(command(fixture.router, "calibration capture empty\n", 8U)));
  fixture.evidence.value = calibration::UltrasonicTrial{100.0F, acquisition::SampleQuality::Uncalibrated, true, true};
  fixture.evidence.sequence = 2U;
  TEST_ASSERT_EQUAL_INT(static_cast<int>(CommandStatus::Accepted),
                        static_cast<int>(command(fixture.router, "calibration capture full\n", 20U)));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(CommandStatus::Accepted),
                        static_cast<int>(command(fixture.router, "calibration validate\n", 21U)));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(CommandStatus::Accepted),
                        static_cast<int>(command(fixture.router, "calibration preview\n", 22U)));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(CommandStatus::Accepted),
                        static_cast<int>(command(fixture.router, "calibration commit\n", 23U)));
  acquisition::ActiveProfileSnapshot active{};
  TEST_ASSERT_TRUE(fixture.profiles.snapshot(acquisition::ProfileTarget::UltrasonicB3, active));
  TEST_ASSERT_TRUE(active.present);
}

void test_first_time_mq_profile_keeps_adc_only_semantics() {
  Fixture fixture{};
  start_common(fixture, "mq4", "10");
  TEST_ASSERT_EQUAL_INT(0, static_cast<int>(command(fixture.router, "calibration criteria reference_mv 3300\n", 4U)));
  TEST_ASSERT_EQUAL_INT(0, static_cast<int>(command(fixture.router, "calibration criteria full_scale_counts 4095\n", 5U)));
  TEST_ASSERT_EQUAL_INT(0, static_cast<int>(command(fixture.router, "calibration criteria divider_numerator 2\n", 6U)));
  TEST_ASSERT_EQUAL_INT(0, static_cast<int>(command(fixture.router, "calibration criteria divider_denominator 1\n", 7U)));
  fixture.evidence.value = calibration::MqTrial{1234U, acquisition::SampleQuality::Uncalibrated, true, true, true};
  fixture.evidence.sequence = 1U;
  finish(fixture, "reference", 8U);
  acquisition::ActiveProfileSnapshot active{};
  TEST_ASSERT_TRUE(fixture.profiles.snapshot(acquisition::ProfileTarget::Mq4, active));
  TEST_ASSERT_TRUE(active.present);
  acquisition::MqAdcProfile profile{};
  TEST_ASSERT_TRUE(fixture.codec.decode(active.encoded, profile));
  TEST_ASSERT_EQUAL_UINT32(4095U, profile.full_scale_counts);
}

void test_first_time_ds_profile_requires_wire_identity_and_reference() {
  Fixture fixture{};
  start_common(fixture, "ds18b20_b3", "10");
  TEST_ASSERT_EQUAL_INT(0, static_cast<int>(command(fixture.router, "calibration criteria resolution_bits 12\n", 4U)));
  TEST_ASSERT_EQUAL_INT(0, static_cast<int>(command(fixture.router, "calibration criteria reference_temperature_centi_c 2350\n", 5U)));
  calibration::Ds18b20Trial trial{2348, acquisition::SampleQuality::Uncalibrated,
                                   true, true, true, true, true,
                                   {0x28U, 1U, 2U, 3U, 4U, 5U, 6U, 0x9eU}, {}, 12U};
  fixture.evidence.value = trial;
  fixture.evidence.sequence = 1U;
  finish(fixture, "reference", 6U);
  acquisition::ActiveProfileSnapshot active{};
  TEST_ASSERT_TRUE(fixture.profiles.snapshot(acquisition::ProfileTarget::Ds18b20B3, active));
  TEST_ASSERT_TRUE(active.present);
  acquisition::Ds18b20Profile profile{};
  TEST_ASSERT_TRUE(fixture.codec.decode(active.encoded, profile));
  TEST_ASSERT_TRUE(profile.has_rom);
  TEST_ASSERT_EQUAL_HEX8(0x28U, profile.rom[0]);
}

void test_first_time_sht31_profile_binds_address_and_offsets() {
  Fixture fixture{};
  start_common(fixture, "sht31_44", "10");
  TEST_ASSERT_EQUAL_INT(0, static_cast<int>(command(fixture.router, "calibration criteria max_temperature_span_centi_c 10\n", 4U)));
  TEST_ASSERT_EQUAL_INT(0, static_cast<int>(command(fixture.router, "calibration criteria max_humidity_span_centi_pct 10\n", 5U)));
  TEST_ASSERT_EQUAL_INT(0, static_cast<int>(command(fixture.router, "calibration criteria reference_temperature_centi_c 2350\n", 6U)));
  TEST_ASSERT_EQUAL_INT(0, static_cast<int>(command(fixture.router, "calibration criteria reference_humidity_centi_pct 5000\n", 7U)));
  fixture.evidence.value = calibration::Sht31Trial{2348, 4998, acquisition::SampleQuality::Uncalibrated, true, true, true, true};
  fixture.evidence.sequence = 1U;
  finish(fixture, "reference", 8U);
  acquisition::ActiveProfileSnapshot active{};
  TEST_ASSERT_TRUE(fixture.profiles.snapshot(acquisition::ProfileTarget::Sht31_44, active));
  TEST_ASSERT_TRUE(active.present);
  acquisition::Sht31Profile profile{};
  TEST_ASSERT_TRUE(fixture.codec.decode(active.encoded, profile));
  TEST_ASSERT_EQUAL_UINT8(0x44U, profile.address);
}

void setup() {
  RUN_TEST(test_first_time_ultrasonic_profile_is_typed_and_persistent);
  RUN_TEST(test_first_time_mq_profile_keeps_adc_only_semantics);
  RUN_TEST(test_first_time_ds_profile_requires_wire_identity_and_reference);
  RUN_TEST(test_first_time_sht31_profile_binds_address_and_offsets);
}

}  // namespace

int main() {
  UNITY_BEGIN();
  setup();
  return UNITY_END();
}
