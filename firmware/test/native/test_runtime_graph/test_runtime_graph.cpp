#include <cstdint>

#include <unity.h>

#include "acquisition/target_runtime.h"
#include "acquisition/boot_identity.h"
#include "acquisition/provenance.h"
#include "acquisition/runtime.h"
#include "config/config_manager.h"
#include "config/config_store.h"

namespace {

using namespace sigap::acquisition;

struct HxFake final : IHx711Reader {
  bool ready() noexcept override { return true; }
  Hx711ReadResult read() noexcept override {
    return Hx711ReadResult{true, false, 100, 1, 0, SampleReason::DataReady};
  }
};

struct UltrasonicFake final : IUltrasonicReader {
  uint32_t trigger_count{0};
  uint64_t last_trigger_us{0};
  bool trigger(uint64_t now_us) noexcept override {
    ++trigger_count;
    last_trigger_us = now_us;
    return true;
  }
  UltrasonicEchoResult poll(uint64_t) noexcept override {
    return UltrasonicEchoResult{UltrasonicEchoStatus::Ready, 100};
  }
};

struct MqFake final : IMqAdcReader {
  MqAdcReadResult read() noexcept override {
    return MqAdcReadResult{true, false, 123, SampleReason::DataReady};
  }
};

struct DsFake final : IDs18b20Reader {
  bool begin_ok{true};
  uint32_t begin_calls{0U};
  uint32_t request_calls{0U};
  bool begin() noexcept override {
    ++begin_calls;
    return begin_ok;
  }
  bool request_conversion() noexcept override {
    ++request_calls;
    return true;
  }
  Ds18b20ReadResult read_conversion() noexcept override {
    return Ds18b20ReadResult{true, true, true, 2500};
  }
};

struct ShtFake final : ISht31Reader {
  Sht31ReadResult read(uint8_t) noexcept override {
    return Sht31ReadResult{true, true, 2500, 5000};
  }
};

struct HeaterFake final : IMq7HeaterRail {
  bool select_high() noexcept override { return true; }
  bool select_low() noexcept override { return true; }
};

void test_target_graph_registers_all_jobs_and_shares_handoff() {
  HxFake hx[3]{};
  UltrasonicFake ultrasonic[3]{};
  MqFake mq[4]{};
  DsFake ds{};
  ShtFake sht{};
  HeaterFake heater{};

  TargetReaderSet readers{{&hx[0], &hx[1], &hx[2]},
                          {&ultrasonic[0], &ultrasonic[1], &ultrasonic[2]},
                          {&mq[0], &mq[1], &mq[2], &mq[3]},
                          &ds,
                          {&sht, &sht},
                          &heater};
  TargetProfileSet profiles{};
  ConfigRef config_ref{};
  config_ref.present = true;
  config_ref.schema_version = 1;
  config_ref.config_version = 2;
  BootId boot{};
  boot.present = true;
  RecordContext context{boot, DeviceId{}, config_ref, nullptr};
  SequenceAllocator sequence{};
  RecordFactory factory(sequence, context);
  SampleRing<TargetAcquisitionGraph::kHandoffCapacity> handoff{};
  TargetAcquisitionGraph graph(factory, readers, profiles, handoff);

  sigap::config::SamplingConfig sampling{};
  sampling.hx711_period_ms = 1;
  sampling.mq_period_ms = 1;
  sampling.ultrasonic_period_ms = 1;
  sampling.ds18b20_period_ms = 1;
  sampling.sht31_period_ms = 1;
  TEST_ASSERT_TRUE(graph.begin(sampling, 0));
  TEST_ASSERT_EQUAL_UINT32(TargetAcquisitionGraph::kJobCount, graph.registered_jobs());

  graph.poll(0);
  TEST_ASSERT_TRUE(graph.enabled());
  TEST_ASSERT_EQUAL_UINT32(10, graph.handoff().size());
  TEST_ASSERT_EQUAL_UINT32(0U, ds.begin_calls);
  TEST_ASSERT_EQUAL_UINT32(0U, ds.request_calls);

  uint64_t previous = 0;
  bool first = true;
  bool saw_hx = false;
  bool saw_mq = false;
  bool saw_sht = false;
  bool saw_sht44_organic = false;
  bool saw_sht45_b3 = false;
  bool saw_ds_profile_missing = false;
  SampleRecord record{};
  while (graph.handoff().try_pop(record) == HandoffStatus::Dequeued) {
    if (!first) TEST_ASSERT_TRUE(record.header.sequence > previous);
    first = false;
    previous = record.header.sequence;
    TEST_ASSERT_TRUE(record.header.config_ref.present);
    TEST_ASSERT_EQUAL_UINT64(2, record.header.config_ref.config_version);
    saw_hx = saw_hx || record.header.sensor == SensorId::Hx711Organic ||
             record.header.sensor == SensorId::Hx711Inorganic ||
             record.header.sensor == SensorId::Hx711B3;
    saw_mq = saw_mq || record.header.kind == SampleKind::MqAdc;
    saw_sht = saw_sht || record.header.kind == SampleKind::Sht31;
    if (record.header.sensor == SensorId::Sht31_44) {
      saw_sht44_organic = true;
      TEST_ASSERT_EQUAL_INT(static_cast<int>(StreamId::Organic),
                            static_cast<int>(record.header.stream));
    }
    if (record.header.sensor == SensorId::Sht31_45) {
      saw_sht45_b3 = true;
      TEST_ASSERT_EQUAL_INT(static_cast<int>(StreamId::B3),
                            static_cast<int>(record.header.stream));
    }
    if (record.header.kind == SampleKind::Ds18b20) {
      saw_ds_profile_missing = true;
      TEST_ASSERT_EQUAL_INT(static_cast<int>(SampleQuality::Uncalibrated),
                            static_cast<int>(record.quality));
      TEST_ASSERT_EQUAL_INT(static_cast<int>(SampleReason::ProfileMissing),
                            static_cast<int>(record.reason));
      TEST_ASSERT_FALSE(record.converted.primary_available);
    }
  }
  TEST_ASSERT_TRUE(saw_hx);
  TEST_ASSERT_TRUE(saw_mq);
  TEST_ASSERT_TRUE(saw_sht);
  TEST_ASSERT_TRUE(saw_sht44_organic);
  TEST_ASSERT_TRUE(saw_sht45_b3);
  TEST_ASSERT_TRUE(saw_ds_profile_missing);
}

void test_target_graph_rejects_missing_reader_dependencies_safely() {
  ConfigRef config_ref{};
  config_ref.present = true;
  config_ref.schema_version = 1U;
  config_ref.config_version = 1U;
  BootId boot{};
  boot.present = true;
  RecordContext context{boot, DeviceId{}, config_ref, nullptr};
  SequenceAllocator sequence{};
  RecordFactory factory(sequence, context);
  TargetReaderSet readers{};
  TargetProfileSet profiles{};
  SampleRing<TargetAcquisitionGraph::kHandoffCapacity> handoff{};
  TargetAcquisitionGraph graph(factory, readers, profiles, handoff);
  sigap::config::SamplingConfig sampling{};

  TEST_ASSERT_FALSE(graph.begin(sampling, 0U));
  TEST_ASSERT_FALSE(graph.enabled());
  TEST_ASSERT_EQUAL_UINT32(0U, graph.registered_jobs());
}

void test_target_graph_keeps_other_channels_alive_when_ds_profile_apply_fails() {
  HxFake hx[3]{};
  UltrasonicFake ultrasonic[3]{};
  MqFake mq[4]{};
  DsFake ds{};
  ds.begin_ok = false;
  ShtFake sht{};
  HeaterFake heater{};
  TargetReaderSet readers{{&hx[0], &hx[1], &hx[2]},
                          {&ultrasonic[0], &ultrasonic[1], &ultrasonic[2]},
                          {&mq[0], &mq[1], &mq[2], &mq[3]},
                          &ds,
                          {&sht, &sht},
                          &heater};
  Ds18b20Profile ds_profile{};
  ds_profile.manifest.validity = ProfileValidity::Valid;
  ds_profile.manifest.target_id =
      static_cast<uint32_t>(ProfileTarget::Ds18b20B3);
  ds_profile.has_rom = true;
  ds_profile.rom[0] = 0x28U;
  TargetProfileSet profiles{};
  profiles.ds18b20 = &ds_profile;
  ConfigRef config_ref{};
  config_ref.present = true;
  config_ref.schema_version = 1U;
  config_ref.config_version = 1U;
  BootId boot{};
  boot.present = true;
  RecordContext context{boot, DeviceId{}, config_ref, nullptr};
  SequenceAllocator sequence{};
  RecordFactory factory(sequence, context);
  SampleRing<TargetAcquisitionGraph::kHandoffCapacity> handoff{};
  TargetAcquisitionGraph graph(factory, readers, profiles, handoff);
  sigap::config::SamplingConfig sampling{};

  TEST_ASSERT_TRUE(graph.begin(sampling, 0U));
  TEST_ASSERT_TRUE(graph.enabled());
  TEST_ASSERT_EQUAL_INT(static_cast<int>(DsChannelState::ProfileApplyFailed),
                        static_cast<int>(graph.ds_channel_state()));
  graph.poll(0U);
  TEST_ASSERT_TRUE(graph.handoff().size() > 0U);
  TEST_ASSERT_EQUAL_UINT32(0U, ds.request_calls);
  SampleRecord record{};
  bool saw_ds_failure = false;
  while (graph.handoff().try_pop(record) == HandoffStatus::Dequeued) {
    if (record.header.kind == SampleKind::Ds18b20) {
      saw_ds_failure = true;
      TEST_ASSERT_EQUAL_INT(static_cast<int>(SampleQuality::Uncalibrated),
                            static_cast<int>(record.quality));
      TEST_ASSERT_EQUAL_INT(static_cast<int>(SampleReason::InvalidProfile),
                            static_cast<int>(record.reason));
    }
  }
  TEST_ASSERT_TRUE(saw_ds_failure);
}

void test_target_graph_uses_strict_ds_channel_when_profile_setup_succeeds() {
  HxFake hx[3]{};
  UltrasonicFake ultrasonic[3]{};
  MqFake mq[4]{};
  DsFake ds{};
  ShtFake sht{};
  HeaterFake heater{};
  TargetReaderSet readers{{&hx[0], &hx[1], &hx[2]},
                          {&ultrasonic[0], &ultrasonic[1], &ultrasonic[2]},
                          {&mq[0], &mq[1], &mq[2], &mq[3]},
                          &ds,
                          {&sht, &sht},
                          &heater};
  Ds18b20Profile ds_profile{};
  ds_profile.manifest.validity = ProfileValidity::Valid;
  ds_profile.manifest.target_id =
      static_cast<uint32_t>(ProfileTarget::Ds18b20B3);
  ds_profile.has_rom = true;
  ds_profile.rom[0] = 0x28U;
  TargetProfileSet profiles{};
  profiles.ds18b20 = &ds_profile;
  ConfigRef config_ref{};
  config_ref.present = true;
  config_ref.schema_version = 1U;
  config_ref.config_version = 1U;
  BootId boot{};
  boot.present = true;
  RecordContext context{boot, DeviceId{}, config_ref, nullptr};
  SequenceAllocator sequence{};
  RecordFactory factory(sequence, context);
  SampleRing<TargetAcquisitionGraph::kHandoffCapacity> handoff{};
  TargetAcquisitionGraph graph(factory, readers, profiles, handoff);
  sigap::config::SamplingConfig sampling{};

  TEST_ASSERT_TRUE(graph.begin(sampling, 0U));
  TEST_ASSERT_EQUAL_UINT32(1U, ds.begin_calls);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(DsChannelState::Ready),
                        static_cast<int>(graph.ds_channel_state()));
  graph.poll(0U);
  TEST_ASSERT_EQUAL_UINT32(1U, ds.request_calls);
}

void test_target_graph_services_ultrasonic_before_one_second_trigger_deadline() {
  HxFake hx[3]{};
  UltrasonicFake ultrasonic[3]{};
  MqFake mq[4]{};
  DsFake ds{};
  ShtFake sht{};
  HeaterFake heater{};
  TargetReaderSet readers{{&hx[0], &hx[1], &hx[2]},
                          {&ultrasonic[0], &ultrasonic[1], &ultrasonic[2]},
                          {&mq[0], &mq[1], &mq[2], &mq[3]},
                          &ds,
                          {&sht, &sht},
                          &heater};
  TargetProfileSet profiles{};
  ConfigRef config_ref{};
  config_ref.present = true;
  config_ref.schema_version = 1U;
  config_ref.config_version = 4U;
  BootId boot{};
  boot.present = true;
  RecordContext context{boot, DeviceId{}, config_ref, nullptr};
  SequenceAllocator sequence{};
  RecordFactory factory(sequence, context);
  SampleRing<TargetAcquisitionGraph::kHandoffCapacity> handoff{};
  TargetAcquisitionGraph graph(factory, readers, profiles, handoff);
  sigap::config::SamplingConfig sampling{};
  sampling.ultrasonic_period_ms = 1000U;
  sampling.hx711_period_ms = 1000U;
  sampling.mq_period_ms = 1000U;
  sampling.ds18b20_period_ms = 1000U;
  sampling.sht31_period_ms = 1000U;

  TEST_ASSERT_TRUE(graph.begin(sampling, 0U));
  graph.poll(0U);
  TEST_ASSERT_EQUAL_UINT32(1U, ultrasonic[0].trigger_count);
  TEST_ASSERT_EQUAL_UINT32(0U, ultrasonic[1].trigger_count);

  graph.poll(4000U);
  TEST_ASSERT_EQUAL_UINT32(1U, ultrasonic[0].trigger_count);
  TEST_ASSERT_TRUE(graph.handoff().size() > 0U);

  graph.poll(500000U);
  TEST_ASSERT_EQUAL_UINT32(1U, ultrasonic[0].trigger_count);
  graph.poll(999999U);
  TEST_ASSERT_EQUAL_UINT32(1U, ultrasonic[0].trigger_count);

  graph.poll(1000000U);
  TEST_ASSERT_EQUAL_UINT32(1U, ultrasonic[0].trigger_count);
  TEST_ASSERT_TRUE(ultrasonic[1].trigger_count >= 1U);

  // The organic channel retries its due request after the shared gate has
  // served the other pending channels, without changing its one-second
  // sample cadence.
  graph.poll(1060000U);
  TEST_ASSERT_EQUAL_UINT32(2U, ultrasonic[0].trigger_count);
  TEST_ASSERT_TRUE(ultrasonic[0].last_trigger_us - ultrasonic[1].last_trigger_us >=
                  UltrasonicEngineeringDefaults::kInterSensorGapUs);
}

void test_acquisition_requires_active_config_provenance() {
  sigap::config::MemoryConfigStore store{};
  const sigap::config::ValidationContext validation{8ULL * 1024ULL * 1024ULL, 0U};
  sigap::config::ConfigManager bootstrap_manager(store, validation);
  TEST_ASSERT_EQUAL_INT(
      static_cast<int>(sigap::config::ConfigBootResult::BootstrapDefaults),
      static_cast<int>(bootstrap_manager.begin()));

  BootRuntimeIdentity bootstrap_identity{};
  RecordContext bootstrap_context{bootstrap_identity.boot_id, DeviceId{},
                                  ConfigRef{}, nullptr};
  RecordFactory bootstrap_factory(bootstrap_identity.sequence, bootstrap_context);
  TargetProfileSet profiles{};
  AcquisitionFoundation bootstrap_runtime(bootstrap_manager, bootstrap_factory,
                                           sigap::board::kN16R8Pins, profiles);
  TEST_ASSERT_FALSE(bootstrap_runtime.begin(
      bootstrap_manager.active(), active_config_ref(bootstrap_manager), 0U));
  TEST_ASSERT_FALSE(bootstrap_runtime.enabled());
  ConfigRef forged_ref{};
  forged_ref.present = true;
  TEST_ASSERT_FALSE(bootstrap_runtime.begin(
      bootstrap_manager.active(), forged_ref, 0U));
  TEST_ASSERT_FALSE(bootstrap_runtime.enabled());

  sigap::config::MemoryConfigStore required_store{};
  required_store.set_provisioned(true);
  sigap::config::ConfigManager required_manager(required_store, validation);
  TEST_ASSERT_EQUAL_INT(
      static_cast<int>(sigap::config::ConfigBootResult::ConfigRequired),
      static_cast<int>(required_manager.begin()));
  BootRuntimeIdentity required_identity{};
  RecordContext required_context{required_identity.boot_id, DeviceId{}, ConfigRef{}, nullptr};
  RecordFactory required_factory(required_identity.sequence, required_context);
  AcquisitionFoundation required_runtime(required_manager, required_factory,
                                          sigap::board::kN16R8Pins, profiles);
  TEST_ASSERT_FALSE(required_runtime.begin(
      required_manager.active(), active_config_ref(required_manager), 0U));
  TEST_ASSERT_FALSE(required_runtime.enabled());

  sigap::config::MemoryConfigStore error_store{};
  error_store.fail_next(sigap::config::MemoryConfigStore::Fault::ReadActivation);
  sigap::config::ConfigManager error_manager(error_store, validation);
  TEST_ASSERT_EQUAL_INT(
      static_cast<int>(sigap::config::ConfigBootResult::StorageError),
      static_cast<int>(error_manager.begin()));
  BootRuntimeIdentity error_identity{};
  RecordContext error_context{error_identity.boot_id, DeviceId{}, ConfigRef{}, nullptr};
  RecordFactory error_factory(error_identity.sequence, error_context);
  AcquisitionFoundation error_runtime(error_manager, error_factory,
                                      sigap::board::kN16R8Pins, profiles);
  TEST_ASSERT_FALSE(error_runtime.begin(
      error_manager.active(), active_config_ref(error_manager), 0U));
  TEST_ASSERT_FALSE(error_runtime.enabled());

  TEST_ASSERT_TRUE(bootstrap_manager.start_draft());
  TEST_ASSERT_TRUE(bootstrap_manager.validate_draft_and_enter());
  TEST_ASSERT_TRUE(bootstrap_manager.preview_draft());
  TEST_ASSERT_TRUE(bootstrap_manager.commit_draft());

  sigap::config::ConfigManager active_manager(store, validation);
  TEST_ASSERT_EQUAL_INT(
      static_cast<int>(sigap::config::ConfigBootResult::ActiveLoaded),
      static_cast<int>(active_manager.begin()));
  BootRuntimeIdentity active_identity{};
  RecordContext active_context{active_identity.boot_id, DeviceId{}, ConfigRef{}, nullptr};
  RecordFactory active_factory(active_identity.sequence, active_context);
  AcquisitionFoundation active_runtime(active_manager, active_factory,
                                        sigap::board::kN16R8Pins, profiles);
  const ConfigRef ref = active_config_ref(active_manager);
  TEST_ASSERT_TRUE(ref.present);
  TEST_ASSERT_TRUE(active_runtime.begin(active_manager.active(), ref, 0U));
  TEST_ASSERT_TRUE(active_runtime.enabled());
  TEST_ASSERT_EQUAL_UINT32(14U, active_runtime.registered_jobs());
}

}  // namespace

void setup() {
  UNITY_BEGIN();
  RUN_TEST(test_target_graph_registers_all_jobs_and_shares_handoff);
  RUN_TEST(test_target_graph_rejects_missing_reader_dependencies_safely);
  RUN_TEST(test_target_graph_keeps_other_channels_alive_when_ds_profile_apply_fails);
  RUN_TEST(test_target_graph_uses_strict_ds_channel_when_profile_setup_succeeds);
  RUN_TEST(test_target_graph_services_ultrasonic_before_one_second_trigger_deadline);
  RUN_TEST(test_acquisition_requires_active_config_provenance);
  UNITY_END();
}

void loop() {}

#ifndef ARDUINO
int main() {
  setup();
  return 0;
}
#endif
