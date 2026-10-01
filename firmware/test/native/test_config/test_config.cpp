#include <algorithm>
#include <cstdio>
#include <cstdint>
#include <cstring>

#include <unity.h>

#include "sigap/board/board_memory.h"
#include "config/config_codec.h"
#include "sigap/config/config_defaults.h"
#include "config/config_manager.h"
#include "config/config_store.h"
#include "config/config_validator.h"

namespace {

using sigap::config::AppConfig;
using sigap::config::ConfigBootResult;
using sigap::config::ConfigCodec;
using sigap::config::ConfigState;
using sigap::config::EncodedConfig;
using sigap::config::MemoryConfigStore;

using sigap::board::BoardMemoryReading;
using sigap::board::BoardMemoryStatus;

constexpr uint64_t kUsableLittlefsBytes{8ULL * 1024ULL * 1024ULL};

sigap::config::ValidationContext validation_context() {
  return {kUsableLittlefsBytes, 0};
}

bool contains_bytes(const EncodedConfig& encoded, const char* text) {
  const std::size_t length = std::strlen(text);
  if (length == 0 || length > encoded.size) return false;
  for (std::size_t offset = 0; offset + length <= encoded.size; ++offset) {
    if (std::memcmp(encoded.bytes.data() + offset, text, length) == 0) {
      return true;
    }
  }
  return false;
}

AppConfig valid_draft(sigap::config::ConfigManager& manager) {
  TEST_ASSERT_TRUE(manager.start_draft());
  AppConfig draft = manager.draft();
  draft.telemetry.summary_period_ms = 30000;
  TEST_ASSERT_TRUE(manager.set_draft(draft));
  TEST_ASSERT_TRUE(manager.validate_draft_and_enter());
  TEST_ASSERT_TRUE(manager.preview_draft());
  return draft;
}

void test_factory_defaults_are_valid() {
  TEST_ASSERT_TRUE(
      sigap::config::validate_config(sigap::config::kFactoryDefaults,
                                     validation_context())
          .ok);
}

void test_n16r8_board_memory_matches_expected_identity() {
  const BoardMemoryReading reading{
      sigap::board::kExpectedFlashBytes, true,
      sigap::board::kExpectedPsramBytes};
  TEST_ASSERT_EQUAL_INT(
      static_cast<int>(BoardMemoryStatus::Match),
      static_cast<int>(sigap::board::evaluate_board_memory(reading)));
}

void test_eight_megabyte_flash_is_rejected() {
  const BoardMemoryReading reading{8ULL * 1024ULL * 1024ULL, true,
                                   sigap::board::kExpectedPsramBytes};
  TEST_ASSERT_EQUAL_INT(
      static_cast<int>(BoardMemoryStatus::FlashSizeMismatch),
      static_cast<int>(sigap::board::evaluate_board_memory(reading)));
}

void test_missing_psram_is_rejected() {
  const BoardMemoryReading reading{sigap::board::kExpectedFlashBytes, false, 0};
  TEST_ASSERT_EQUAL_INT(
      static_cast<int>(BoardMemoryStatus::PsramMissing),
      static_cast<int>(sigap::board::evaluate_board_memory(reading)));
}

void test_wrong_psram_size_is_rejected() {
  const BoardMemoryReading reading{sigap::board::kExpectedFlashBytes, true,
                                   2ULL * 1024ULL * 1024ULL};
  TEST_ASSERT_EQUAL_INT(
      static_cast<int>(BoardMemoryStatus::PsramSizeMismatch),
      static_cast<int>(sigap::board::evaluate_board_memory(reading)));
}

void test_unsupported_schema_is_rejected() {
  AppConfig config = sigap::config::kFactoryDefaults;
  config.schema_version = 99;
  TEST_ASSERT_EQUAL_INT(
      static_cast<int>(sigap::config::ConfigValidationError::SchemaUnsupported),
      static_cast<int>(sigap::config::validate_config(config, validation_context())
                           .error));
}

void test_zero_or_invalid_period_is_rejected() {
  AppConfig config = sigap::config::kFactoryDefaults;
  config.sampling.hx711_period_ms = 0;
  TEST_ASSERT_FALSE(
      sigap::config::validate_config(config, validation_context()).ok);

  config = sigap::config::kFactoryDefaults;
  config.sampling.ultrasonic_period_ms = 59;
  TEST_ASSERT_EQUAL_INT(
      static_cast<int>(sigap::config::ConfigValidationError::InvalidPeriod),
      static_cast<int>(sigap::config::validate_config(config, validation_context())
                           .error));
}

void test_summary_period_range_is_rejected() {
  AppConfig config = sigap::config::kFactoryDefaults;
  config.telemetry.summary_period_ms = 4999;
  TEST_ASSERT_FALSE(
      sigap::config::validate_config(config, validation_context()).ok);
  config.telemetry.summary_period_ms = 300001;
  TEST_ASSERT_FALSE(
      sigap::config::validate_config(config, validation_context()).ok);
}

void test_retry_minimum_above_maximum_is_rejected() {
  AppConfig config = sigap::config::kFactoryDefaults;
  config.telemetry.retry_min_ms = 5000;
  config.telemetry.retry_max_ms = 1000;
  TEST_ASSERT_EQUAL_INT(
      static_cast<int>(sigap::config::ConfigValidationError::RetryOrder),
      static_cast<int>(sigap::config::validate_config(config, validation_context())
                           .error));
}

void test_batch_count_bounds_are_rejected() {
  AppConfig config = sigap::config::kFactoryDefaults;
  config.telemetry.max_records_per_batch = 0;
  TEST_ASSERT_FALSE(
      sigap::config::validate_config(config, validation_context()).ok);
  config.telemetry.max_records_per_batch = 129;
  TEST_ASSERT_FALSE(
      sigap::config::validate_config(config, validation_context()).ok);
}

void test_flash_quota_reserve_is_enforced() {
  AppConfig config = sigap::config::kFactoryDefaults;
  config.storage.flash_pending_quota_bytes = 6ULL * 1024ULL * 1024ULL + 1ULL;
  TEST_ASSERT_EQUAL_INT(
      static_cast<int>(sigap::config::ConfigValidationError::FlashQuota),
      static_cast<int>(sigap::config::validate_config(config, validation_context())
                           .error));
}

void test_flash_quota_uses_external_usable_capacity() {
  AppConfig config = sigap::config::kFactoryDefaults;
  const sigap::config::ValidationContext large_context{
      9ULL * 1024ULL * 1024ULL, 0};
  config.storage.flash_pending_quota_bytes = 7ULL * 1024ULL * 1024ULL;
  TEST_ASSERT_TRUE(
      sigap::config::validate_config(config, large_context).ok);

  const sigap::config::ValidationContext smaller_context{
      8ULL * 1024ULL * 1024ULL, 0};
  TEST_ASSERT_EQUAL_INT(
      static_cast<int>(sigap::config::ConfigValidationError::FlashQuota),
      static_cast<int>(sigap::config::validate_config(config, smaller_context)
                           .error));
}

void test_sd_quota_is_checked_when_capacity_is_known() {
  AppConfig config = sigap::config::kFactoryDefaults;
  const sigap::config::ValidationContext context{
      kUsableLittlefsBytes, 21ULL * 1024ULL * 1024ULL * 1024ULL};
  config.storage.sd_pending_quota_bytes = context.usable_sd_bytes + 1U;
  TEST_ASSERT_EQUAL_INT(
      static_cast<int>(sigap::config::ConfigValidationError::SdQuota),
      static_cast<int>(sigap::config::validate_config(config, context).error));
}

void test_configuration_memory_footprint_is_bounded() {
  std::printf("memory sizeof(AppConfig)=%zu EncodedConfig=%zu ConfigManager=%zu\\n",
              sizeof(AppConfig), sizeof(EncodedConfig),
              sizeof(sigap::config::ConfigManager));
  TEST_ASSERT_TRUE(sizeof(AppConfig) <= 128U);
  TEST_ASSERT_TRUE(sizeof(EncodedConfig) <= 256U);
  TEST_ASSERT_TRUE(sizeof(sigap::config::ConfigManager) <= 1024U);
}

void test_deterministic_encoding_and_hash() {
  ConfigCodec codec;
  EncodedConfig first{};
  EncodedConfig second{};
  TEST_ASSERT_TRUE(codec.encode(sigap::config::kFactoryDefaults, first));
  TEST_ASSERT_TRUE(codec.encode(sigap::config::kFactoryDefaults, second));
  TEST_ASSERT_EQUAL_UINT32(first.size, second.size);
  TEST_ASSERT_EQUAL_UINT8_ARRAY(first.bytes.data(), second.bytes.data(),
                                first.size);
  TEST_ASSERT_EQUAL_UINT8_ARRAY(first.hash().data(), second.hash().data(),
                                first.hash().size());
}

void test_codec_round_trip() {
  ConfigCodec codec;
  AppConfig original = sigap::config::kFactoryDefaults;
  original.config_version = 7;
  original.sampling.sht31_period_ms = 2500;
  EncodedConfig encoded{};
  AppConfig decoded{};
  sigap::config::ConfigHash hash{};
  TEST_ASSERT_TRUE(codec.encode(original, encoded));
  TEST_ASSERT_TRUE(codec.decode(encoded, decoded, hash));
  TEST_ASSERT_EQUAL_UINT64(original.config_version, decoded.config_version);
  TEST_ASSERT_EQUAL_UINT32(original.sampling.sht31_period_ms,
                           decoded.sampling.sht31_period_ms);
  TEST_ASSERT_EQUAL_UINT8_ARRAY(encoded.hash().data(), hash.data(), hash.size());
}

void test_malformed_truncated_or_unsupported_codec_is_rejected() {
  ConfigCodec codec;
  EncodedConfig encoded{};
  AppConfig decoded{};
  sigap::config::ConfigHash hash{};
  TEST_ASSERT_TRUE(codec.encode(sigap::config::kFactoryDefaults, encoded));
  TEST_ASSERT_FALSE(codec.decode(encoded.bytes.data(), encoded.size - 1U,
                                 decoded, hash));
  encoded.bytes[0] ^= 0x01U;
  TEST_ASSERT_FALSE(codec.decode(encoded, decoded, hash));
  TEST_ASSERT_TRUE(codec.encode(sigap::config::kFactoryDefaults, encoded));
  encoded.bytes[18] = 0xffU;
  encoded.bytes[19] = 0xffU;
  std::copy_n(encoded.bytes.begin() + sigap::config::kConfigEnvelopeBytes,
              sigap::config::kConfigHashBytes,
              encoded.bytes.begin() + sigap::config::kConfigEnvelopeBytes);
  TEST_ASSERT_FALSE(codec.decode(encoded, decoded, hash));
}

void test_codec_does_not_persist_secret_boundary() {
  ConfigCodec codec;
  EncodedConfig encoded{};
  TEST_ASSERT_TRUE(codec.encode(sigap::config::kFactoryDefaults, encoded));
  TEST_ASSERT_FALSE(contains_bytes(encoded, "wifi-password-fixture"));
  TEST_ASSERT_FALSE(contains_bytes(encoded, "device-token-fixture"));
}

void test_bootstrap_defaults_on_unprovisioned_device() {
  MemoryConfigStore store;
  sigap::config::ConfigManager manager(store, validation_context());
  TEST_ASSERT_EQUAL_INT(
      static_cast<int>(ConfigBootResult::BootstrapDefaults),
      static_cast<int>(manager.begin()));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(ConfigState::Draft),
                        static_cast<int>(manager.state()));
  TEST_ASSERT_TRUE(manager.has_draft());
}

void test_successful_commit_requires_reboot_and_keeps_active_unchanged() {
  MemoryConfigStore store;
  sigap::config::ConfigManager manager(store, validation_context());
  manager.begin();
  const AppConfig active_before = manager.active();
  const AppConfig draft = valid_draft(manager);
  TEST_ASSERT_TRUE(manager.commit_draft());
  TEST_ASSERT_EQUAL_INT(static_cast<int>(ConfigState::RebootRequired),
                        static_cast<int>(manager.state()));
  TEST_ASSERT_EQUAL_UINT64(active_before.config_version,
                           manager.active().config_version);
  TEST_ASSERT_NOT_EQUAL(active_before.telemetry.summary_period_ms,
                        draft.telemetry.summary_period_ms);
}

void test_new_boot_loads_committed_version_and_hash() {
  MemoryConfigStore store;
  sigap::config::ConfigManager first(store, validation_context());
  first.begin();
  valid_draft(first);
  TEST_ASSERT_TRUE(first.commit_draft());
  sigap::config::ConfigManager reboot(store, validation_context());
  TEST_ASSERT_EQUAL_INT(static_cast<int>(ConfigBootResult::ActiveLoaded),
                        static_cast<int>(reboot.begin()));
  TEST_ASSERT_EQUAL_UINT64(1, reboot.active().config_version);
  TEST_ASSERT_EQUAL_UINT32(30000, reboot.active().telemetry.summary_period_ms);
  ConfigCodec codec;
  EncodedConfig expected{};
  TEST_ASSERT_TRUE(codec.encode(first.draft(), expected));
  TEST_ASSERT_EQUAL_UINT8_ARRAY(expected.hash().data(),
                                reboot.identity().hash.data(),
                                reboot.identity().hash.size());
}

void test_config_version_increments_once_per_reboot_commit() {
  MemoryConfigStore store;
  sigap::config::ConfigManager first(store, validation_context());
  first.begin();
  valid_draft(first);
  TEST_ASSERT_TRUE(first.commit_draft());
  sigap::config::ConfigManager second(store, validation_context());
  second.begin();
  valid_draft(second);
  TEST_ASSERT_TRUE(second.commit_draft());
  sigap::config::ConfigManager third(store, validation_context());
  TEST_ASSERT_EQUAL_INT(static_cast<int>(ConfigBootResult::ActiveLoaded),
                        static_cast<int>(third.begin()));
  TEST_ASSERT_EQUAL_UINT64(2, third.active().config_version);
}

void test_failed_write_leaves_previous_slot_authoritative() {
  MemoryConfigStore store;
  sigap::config::ConfigManager manager(store, validation_context());
  manager.begin();
  valid_draft(manager);
  store.fail_next(MemoryConfigStore::Fault::WriteSlot);
  TEST_ASSERT_FALSE(manager.commit_draft());
  TEST_ASSERT_EQUAL_INT(static_cast<int>(ConfigState::Draft),
                        static_cast<int>(manager.state()));
  TEST_ASSERT_FALSE(store.slot_present(sigap::config::ConfigSlot::A));
}

void test_failed_readback_leaves_previous_slot_authoritative() {
  MemoryConfigStore store;
  sigap::config::ConfigManager manager(store, validation_context());
  manager.begin();
  valid_draft(manager);
  store.fail_next(MemoryConfigStore::Fault::ReadSlot);
  TEST_ASSERT_FALSE(manager.commit_draft());
  TEST_ASSERT_EQUAL_INT(static_cast<int>(ConfigState::Draft),
                        static_cast<int>(manager.state()));
  sigap::config::ConfigSlot pointer{};
  TEST_ASSERT_FALSE(store.read_active_pointer(pointer));
}

void test_corrupt_new_slot_leaves_previous_pointer_active() {
  MemoryConfigStore store;
  sigap::config::ConfigManager manager(store, validation_context());
  manager.begin();
  valid_draft(manager);
  TEST_ASSERT_TRUE(manager.commit_draft());

  sigap::config::ConfigManager second(store, validation_context());
  second.begin();
  valid_draft(second);
  store.fail_next(MemoryConfigStore::Fault::CorruptAfterWrite);
  TEST_ASSERT_FALSE(second.commit_draft());

  sigap::config::ConfigManager reboot(store, validation_context());
  TEST_ASSERT_EQUAL_INT(static_cast<int>(ConfigBootResult::ActiveLoaded),
                        static_cast<int>(reboot.begin()));
  TEST_ASSERT_EQUAL_UINT64(1, reboot.active().config_version);
}

void test_pointer_write_failure_keeps_old_active_config() {
  MemoryConfigStore store;
  sigap::config::ConfigManager first(store, validation_context());
  first.begin();
  valid_draft(first);
  TEST_ASSERT_TRUE(first.commit_draft());

  sigap::config::ConfigManager second(store, validation_context());
  second.begin();
  valid_draft(second);
  store.fail_next(MemoryConfigStore::Fault::WritePointer);
  TEST_ASSERT_FALSE(second.commit_draft());

  sigap::config::ConfigManager reboot(store, validation_context());
  TEST_ASSERT_EQUAL_INT(static_cast<int>(ConfigBootResult::ActiveLoaded),
                        static_cast<int>(reboot.begin()));
  TEST_ASSERT_EQUAL_UINT64(1, reboot.active().config_version);
}

void test_corrupt_active_slot_falls_back_to_previous_valid_slot() {
  MemoryConfigStore store;
  sigap::config::ConfigManager first(store, validation_context());
  first.begin();
  valid_draft(first);
  TEST_ASSERT_TRUE(first.commit_draft());
  sigap::config::ConfigManager second(store, validation_context());
  second.begin();
  valid_draft(second);
  TEST_ASSERT_TRUE(second.commit_draft());

  store.corrupt_slot(sigap::config::ConfigSlot::B);
  sigap::config::ConfigManager reboot(store, validation_context());
  TEST_ASSERT_EQUAL_INT(static_cast<int>(ConfigBootResult::FallbackLoaded),
                        static_cast<int>(reboot.begin()));
  TEST_ASSERT_TRUE(reboot.fallback_used());
  TEST_ASSERT_EQUAL_UINT64(1, reboot.active().config_version);
}

void test_provisioned_device_without_valid_snapshot_requires_config() {
  MemoryConfigStore store;
  store.set_provisioned(true);
  sigap::config::ConfigManager manager(store, validation_context());
  TEST_ASSERT_EQUAL_INT(static_cast<int>(ConfigBootResult::ConfigRequired),
                        static_cast<int>(manager.begin()));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(ConfigState::ConfigRequired),
                        static_cast<int>(manager.state()));
}

void test_provisioned_device_with_both_corrupt_slots_requires_config() {
  MemoryConfigStore store;
  sigap::config::ConfigManager first(store, validation_context());
  first.begin();
  valid_draft(first);
  TEST_ASSERT_TRUE(first.commit_draft());

  sigap::config::ConfigManager second(store, validation_context());
  second.begin();
  valid_draft(second);
  TEST_ASSERT_TRUE(second.commit_draft());

  store.corrupt_slot(sigap::config::ConfigSlot::A);
  store.corrupt_slot(sigap::config::ConfigSlot::B);
  sigap::config::ConfigManager reboot(store, validation_context());
  TEST_ASSERT_EQUAL_INT(static_cast<int>(ConfigBootResult::ConfigRequired),
                        static_cast<int>(reboot.begin()));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(ConfigState::ConfigRequired),
                        static_cast<int>(reboot.state()));
}

void test_draft_is_isolated_from_active_config() {
  MemoryConfigStore store;
  sigap::config::ConfigManager manager(store, validation_context());
  manager.begin();
  const uint32_t active_summary = manager.active().telemetry.summary_period_ms;
  TEST_ASSERT_TRUE(manager.start_draft());
  AppConfig draft = manager.draft();
  draft.telemetry.summary_period_ms = 60000;
  TEST_ASSERT_TRUE(manager.set_draft(draft));
  TEST_ASSERT_EQUAL_UINT32(active_summary,
                           manager.active().telemetry.summary_period_ms);
}

void test_invalid_draft_cannot_commit() {
  MemoryConfigStore store;
  sigap::config::ConfigManager manager(store, validation_context());
  manager.begin();
  TEST_ASSERT_TRUE(manager.start_draft());
  AppConfig draft = manager.draft();
  draft.telemetry.retry_min_ms = 9000;
  draft.telemetry.retry_max_ms = 1000;
  TEST_ASSERT_TRUE(manager.set_draft(draft));
  TEST_ASSERT_FALSE(manager.validate_draft_and_enter());
  TEST_ASSERT_FALSE(manager.commit_draft());
  TEST_ASSERT_EQUAL_INT(static_cast<int>(ConfigState::Draft),
                        static_cast<int>(manager.state()));
}

void setUp() {}
void tearDown() {}

}  // namespace

void setup() {
  UNITY_BEGIN();
  RUN_TEST(test_factory_defaults_are_valid);
  RUN_TEST(test_n16r8_board_memory_matches_expected_identity);
  RUN_TEST(test_eight_megabyte_flash_is_rejected);
  RUN_TEST(test_missing_psram_is_rejected);
  RUN_TEST(test_wrong_psram_size_is_rejected);
  RUN_TEST(test_unsupported_schema_is_rejected);
  RUN_TEST(test_zero_or_invalid_period_is_rejected);
  RUN_TEST(test_summary_period_range_is_rejected);
  RUN_TEST(test_retry_minimum_above_maximum_is_rejected);
  RUN_TEST(test_batch_count_bounds_are_rejected);
  RUN_TEST(test_flash_quota_reserve_is_enforced);
  RUN_TEST(test_flash_quota_uses_external_usable_capacity);
  RUN_TEST(test_sd_quota_is_checked_when_capacity_is_known);
  RUN_TEST(test_configuration_memory_footprint_is_bounded);
  RUN_TEST(test_deterministic_encoding_and_hash);
  RUN_TEST(test_codec_round_trip);
  RUN_TEST(test_malformed_truncated_or_unsupported_codec_is_rejected);
  RUN_TEST(test_codec_does_not_persist_secret_boundary);
  RUN_TEST(test_bootstrap_defaults_on_unprovisioned_device);
  RUN_TEST(test_successful_commit_requires_reboot_and_keeps_active_unchanged);
  RUN_TEST(test_new_boot_loads_committed_version_and_hash);
  RUN_TEST(test_config_version_increments_once_per_reboot_commit);
  RUN_TEST(test_failed_write_leaves_previous_slot_authoritative);
  RUN_TEST(test_failed_readback_leaves_previous_slot_authoritative);
  RUN_TEST(test_corrupt_new_slot_leaves_previous_pointer_active);
  RUN_TEST(test_pointer_write_failure_keeps_old_active_config);
  RUN_TEST(test_corrupt_active_slot_falls_back_to_previous_valid_slot);
  RUN_TEST(test_provisioned_device_without_valid_snapshot_requires_config);
  RUN_TEST(test_provisioned_device_with_both_corrupt_slots_requires_config);
  RUN_TEST(test_draft_is_isolated_from_active_config);
  RUN_TEST(test_invalid_draft_cannot_commit);
  UNITY_END();
}

void loop() {}

#ifndef ARDUINO
int main() {
  setup();
  return 0;
}
#endif
