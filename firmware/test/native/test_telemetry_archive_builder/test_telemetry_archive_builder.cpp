#include <cstdint>
#include <cstring>
#include <string_view>

#include <unity.h>

#include "storage/archive_codec.h"
#include "telemetry_archive/ack_journal_store.h"
#include "telemetry_archive/batch_store.h"
#include "telemetry_archive/runtime.h"

namespace {

using namespace sigap;
using namespace sigap::acquisition;
using namespace sigap::storage;
using namespace sigap::telemetry_archive;

DeviceId device() {
  DeviceId value{};
  value.present = true;
  for (std::size_t index = 0U; index < value.bytes.size(); ++index) {
    value.bytes[index] = static_cast<uint8_t>(index + 1U);
  }
  return value;
}

BootId boot() {
  BootId value{};
  value.present = true;
  for (std::size_t index = 0U; index < value.bytes.size(); ++index) {
    value.bytes[index] = static_cast<uint8_t>(0xa0U + index);
  }
  return value;
}

ImmutableRecordBytes sample(uint64_t sequence) {
  SampleRecord value{};
  value.header.device_id = device();
  value.header.boot_id = boot();
  value.header.sequence = sequence;
  value.header.monotonic_us = 1234U;
  value.header.sensor = SensorId::Mq4;
  value.header.stream = StreamId::Organic;
  value.header.kind = SampleKind::MqAdc;
  value.header.config_ref.present = true;
  value.header.config_ref.schema_version = 1U;
  value.header.config_ref.config_version = 2U;
  value.quality = SampleQuality::Uncalibrated;
  value.reason = SampleReason::ProfileMissing;
  value.raw.present = true;
  value.raw.adc_counts = 1200U;
  ImmutableRecordBytes encoded{};
  TEST_ASSERT_TRUE(RecordCodec::encode_sample(value, encoded));
  return encoded;
}

void test_builder_emits_frozen_envelope_and_stable_identity() {
  BatchJsonBuilder builder{};
  ImmutableBatch batch{};
  TEST_ASSERT_TRUE(builder.build(sample(7U), batch));
  const auto body = batch.exact_body();
  TEST_ASSERT_TRUE(body.find("\"schema_version\":3") != std::string_view::npos);
  TEST_ASSERT_TRUE(body.find("\"samples\":[{") != std::string_view::npos);
  TEST_ASSERT_TRUE(body.find("\"results\":[]") != std::string_view::npos);
  TEST_ASSERT_TRUE(body.find("\"events\":[]") != std::string_view::npos);
  TEST_ASSERT_TRUE(body.find("\"metadata\":[]") != std::string_view::npos);
  TEST_ASSERT_TRUE(body.find("\"records\":[") == std::string_view::npos);
  TEST_ASSERT_TRUE(body.find("\"record_id\":\"0102030405060708090a0b0c0d0e0f10/a0a1a2a3a4a5a6a7a8a9aaabacadaeaf/7\"") != std::string_view::npos);
  TEST_ASSERT_TRUE(batch.id().find("/batch") != std::string_view::npos);
  TEST_ASSERT_TRUE(body.find("\"record_hash\":\"sha256:") != std::string_view::npos);
  TEST_ASSERT_TRUE(batch.body_hash_matches());
}

void test_record_hash_insertion_has_no_padding_byte() {
  BatchJsonBuilder builder{};
  ImmutableBatch batch{};
  TEST_ASSERT_TRUE(builder.build(sample(8U), batch));
  const auto body = batch.exact_body();
  const auto marker = body.find("\"record_hash\":\"sha256:");
  TEST_ASSERT_TRUE(marker != std::string_view::npos);
  const auto hash_start = marker + std::string_view("\"record_hash\":\"sha256:").size();
  const auto quote = body.find('"', hash_start);
  TEST_ASSERT_TRUE(quote != std::string_view::npos);
  TEST_ASSERT_EQUAL_UINT32(64U, static_cast<uint32_t>(quote - hash_start));
  TEST_ASSERT_TRUE(body.find("\\0") == std::string_view::npos);
}

}  // namespace

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_builder_emits_frozen_envelope_and_stable_identity);
  RUN_TEST(test_record_hash_insertion_has_no_padding_byte);
  return UNITY_END();
}
