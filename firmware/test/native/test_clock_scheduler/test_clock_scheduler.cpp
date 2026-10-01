#include <cstdint>

#include <unity.h>

#include "acquisition/arduino_adapters.h"
#include "acquisition/clock.h"
#include "acquisition/scheduler.h"

namespace {

using sigap::acquisition::CooperativeScheduler;
using sigap::acquisition::InjectedMonotonicClock;

struct ClockFixture {
  uint64_t value{0};
  static uint64_t read(void* context) noexcept {
    return static_cast<ClockFixture*>(context)->value;
  }
};

void test_injected_clock_uses_a_64_bit_source() {
  ClockFixture fixture{0x100000000ULL + 17U};
  const InjectedMonotonicClock clock(&ClockFixture::read, &fixture);
  TEST_ASSERT_EQUAL_UINT64(0x100000011ULL, clock.now_us());
  fixture.value += 9U;
  TEST_ASSERT_EQUAL_UINT64(0x10000001aULL, clock.now_us());
}

struct JobFixture {
  uint32_t calls{0};
  uint64_t last_now{0};
  static void run(void* context, uint64_t now_us) noexcept {
    auto* fixture = static_cast<JobFixture*>(context);
    ++fixture->calls;
    fixture->last_now = now_us;
  }
};

void test_scheduler_preserves_deadline_phase_after_late_poll() {
  CooperativeScheduler scheduler;
  JobFixture fixture{};
  TEST_ASSERT_TRUE(
      scheduler.add_job(100U, &JobFixture::run, &fixture, 100U));

  scheduler.poll(100U);
  scheduler.poll(350U);
  TEST_ASSERT_EQUAL_UINT32(2U, fixture.calls);
  TEST_ASSERT_EQUAL_UINT64(350U, fixture.last_now);

  // The next deadline remains 400, not 450 (now + period).
  scheduler.poll(399U);
  TEST_ASSERT_EQUAL_UINT32(2U, fixture.calls);
  scheduler.poll(400U);
  TEST_ASSERT_EQUAL_UINT32(3U, fixture.calls);
}

void test_scheduler_skips_missed_periods_without_burst_callbacks() {
  CooperativeScheduler scheduler;
  JobFixture fixture{};
  TEST_ASSERT_TRUE(
      scheduler.add_job(10U, &JobFixture::run, &fixture, 0U));

  scheduler.poll(0U);
  scheduler.poll(1000U);
  TEST_ASSERT_EQUAL_UINT32(2U, fixture.calls);
  scheduler.poll(1010U);
  TEST_ASSERT_EQUAL_UINT32(3U, fixture.calls);
}

void test_scheduler_disables_job_when_deadline_would_overflow() {
  CooperativeScheduler scheduler;
  JobFixture fixture{};
  constexpr uint64_t max = UINT64_MAX;
  TEST_ASSERT_TRUE(
      scheduler.add_job(2U, &JobFixture::run, &fixture, max - 1U));

  scheduler.poll(max - 1U);
  TEST_ASSERT_EQUAL_UINT32(1U, fixture.calls);
  TEST_ASSERT_TRUE(scheduler.deadline_overflowed());

  scheduler.poll(max);
  TEST_ASSERT_EQUAL_UINT32(1U, fixture.calls);
}

void test_scheduler_rejects_backward_time_without_running_jobs() {
  CooperativeScheduler scheduler;
  JobFixture fixture{};
  TEST_ASSERT_TRUE(
      scheduler.add_job(10U, &JobFixture::run, &fixture, 10U));

  scheduler.poll(10U);
  scheduler.poll(9U);
  TEST_ASSERT_EQUAL_UINT32(1U, fixture.calls);
}

void setUp() {}
void tearDown() {}

}  // namespace

void setup() {
  UNITY_BEGIN();
  RUN_TEST(test_injected_clock_uses_a_64_bit_source);
  RUN_TEST(test_scheduler_preserves_deadline_phase_after_late_poll);
  RUN_TEST(test_scheduler_skips_missed_periods_without_burst_callbacks);
  RUN_TEST(test_scheduler_disables_job_when_deadline_would_overflow);
  RUN_TEST(test_scheduler_rejects_backward_time_without_running_jobs);
  UNITY_END();
}

int main() {
  setup();
  return 0;
}
