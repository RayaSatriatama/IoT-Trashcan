#include <cstdint>

#include <unity.h>

#include "acquisition/record.h"
#include "acquisition/sensors/ultrasonic.h"

namespace {

using namespace sigap::acquisition;

RecordContext context() {
  RecordContext result{};
  result.config_ref.present = true;
  result.config_ref.schema_version = 1U;
  result.config_ref.config_version = 1U;
  result.boot_id.present = true;
  return result;
}

struct EchoReader final : IUltrasonicReader {
  bool trigger_ok{true};
  uint32_t trigger_count{0};
  uint64_t last_trigger_us{0};
  UltrasonicEchoResult next{UltrasonicEchoStatus::Pending, 0U};

  bool trigger(uint64_t now_us) noexcept override {
    ++trigger_count;
    last_trigger_us = now_us;
    return trigger_ok;
  }

  UltrasonicEchoResult poll(uint64_t) noexcept override { return next; }
};

UltrasonicProfile valid_profile() {
  UltrasonicProfile profile{};
  profile.manifest.validity = ProfileValidity::Valid;
  profile.manifest.target_id = static_cast<uint32_t>(ProfileTarget::UltrasonicOrganic);
  profile.d_empty_mm = 100.0F;
  profile.d_full_mm = 20.0F;
  profile.echo_mm_per_us = 0.1F;
  return profile;
}

void test_ultrasonic_echo_is_serviced_before_sample_period() {
  SequenceAllocator sequence{};
  RecordFactory factory(sequence, context());
  EchoReader reader{};
  reader.next = {UltrasonicEchoStatus::Pending, 0U};
  UltrasonicProfile profile = valid_profile();
  UltrasonicAcquirer acquirer(StreamId::Organic, reader, factory, &profile, 30000U);
  SampleRecord record{};

  // The configured sample cadence is one second. The echo service still runs
  // at loop cadence and completes this attempt after four milliseconds.
  TEST_ASSERT_EQUAL_INT(static_cast<int>(DriverPollStatus::NoAttempt),
                        static_cast<int>(acquirer.request_sample(0U)));
  TEST_ASSERT_EQUAL_UINT32(1U, reader.trigger_count);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(DriverPollStatus::NoAttempt),
                        static_cast<int>(acquirer.service(1000U, record)));
  reader.next = {UltrasonicEchoStatus::Ready, 500U};
  TEST_ASSERT_EQUAL_INT(static_cast<int>(DriverPollStatus::Produced),
                        static_cast<int>(acquirer.service(4000U, record)));
  TEST_ASSERT_TRUE(record.converted.primary_available);
  TEST_ASSERT_EQUAL_UINT32(1U, reader.trigger_count);

  // The acquirer is ready for the next scheduled request, but a scheduler
  // remains responsible for enforcing the one-second sample cadence.
  TEST_ASSERT_EQUAL_INT(static_cast<int>(DriverPollStatus::NoAttempt),
                        static_cast<int>(acquirer.request_sample(500000U)));
  TEST_ASSERT_EQUAL_UINT32(2U, reader.trigger_count);
}

void test_ultrasonic_gate_blocks_cross_channel_trigger_for_sixty_ms() {
  SequenceAllocator sequence{};
  RecordFactory factory(sequence, context());
  EchoReader first{};
  EchoReader second{};
  UltrasonicProfile first_profile = valid_profile();
  UltrasonicProfile second_profile = valid_profile();
  UltrasonicTriggerGate gate{};
  UltrasonicAcquirer organic(StreamId::Organic, first, factory, &first_profile,
                             30000U, gate);
  UltrasonicAcquirer inorganic(StreamId::Inorganic, second, factory, &second_profile,
                               30000U, gate);
  SampleRecord record{};

  TEST_ASSERT_EQUAL_INT(static_cast<int>(DriverPollStatus::NoAttempt),
                        static_cast<int>(organic.request_sample(0U)));
  TEST_ASSERT_EQUAL_UINT32(1U, first.trigger_count);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(DriverPollStatus::NoAttempt),
                        static_cast<int>(inorganic.request_sample(59999U)));
  TEST_ASSERT_EQUAL_UINT32(0U, second.trigger_count);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(DriverPollStatus::NoAttempt),
                        static_cast<int>(inorganic.request_sample(60000U)));
  TEST_ASSERT_EQUAL_UINT32(1U, second.trigger_count);
  (void)record;
}

void test_ultrasonic_timeout_produces_record_without_waiting_for_next_period() {
  SequenceAllocator sequence{};
  RecordFactory factory(sequence, context());
  EchoReader reader{};
  reader.next = {UltrasonicEchoStatus::Pending, 0U};
  UltrasonicProfile profile = valid_profile();
  UltrasonicAcquirer acquirer(StreamId::Organic, reader, factory, &profile, 30000U);
  SampleRecord record{};

  acquirer.request_sample(1000000U);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(DriverPollStatus::Produced),
                        static_cast<int>(acquirer.service(1030000U, record)));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(SampleQuality::Timeout),
                        static_cast<int>(record.quality));
  TEST_ASSERT_FALSE(record.raw.present);
  TEST_ASSERT_FALSE(record.converted.primary_available);
}

void setUp() {}
void tearDown() {}

}  // namespace

void setup() {
  UNITY_BEGIN();
  RUN_TEST(test_ultrasonic_echo_is_serviced_before_sample_period);
  RUN_TEST(test_ultrasonic_gate_blocks_cross_channel_trigger_for_sixty_ms);
  RUN_TEST(test_ultrasonic_timeout_produces_record_without_waiting_for_next_period);
  UNITY_END();
}

int main() {
  setup();
  return 0;
}
